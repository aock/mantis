#include "mantis.h"
#include "Delaunay_psm.h"

#include <queue>
#include <unordered_set>
#include <algorithm>
#include <numeric>
#include <thread>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define MANTIS_HAS_NEON
#elif defined(__AVX__)
#define MANTIS_HAS_AVX
#ifdef __AVX512F__
#define MANTIS_HAS_AVX512
#endif
#else
#error "Mantis: No SIMD support detected, platform not supported."
#endif

#ifdef MANTIS_HAS_NEON
#include <arm_neon.h>
#endif

#ifdef MANTIS_HAS_AVX
#include <immintrin.h>
#endif

//#define DEBUG_MANTIS

#ifdef MANTIS_HAS_NEON
// float32x4_t and int32x4_t are already defined in arm_neon.h
using mask4_t = uint32x4_t;
#endif

#ifdef MANTIS_HAS_AVX
using float32x4_t = __m128;
using int32x4_t = __m128i;
using mask4_t = __m128i;
#endif

#ifdef MANTIS_HAS_AVX512
using float32x16_t = __m512;
using int32x16_t = __m512i;
using mask16_t = __mmask16;
#endif

#ifdef MANTIS_HAS_AVX512
constexpr size_t SimdWidth = 16;
using float32xN_t = float32x16_t;
using int32xN_t = int32x16_t;
using maskN_t = mask16_t;
#else
// For both SSE, AVX and NEON
constexpr size_t SimdWidth = 4;
using float32xN_t = float32x4_t;
using int32xN_t = int32x4_t;
using maskN_t = mask4_t;
#endif

namespace mantis {

using index_t = GEO::index_t;

// ============================= MISC STRUCTS ===============================

struct PackedEdge {
    float32xN_t min_x;
    float32xN_t start[3];
    float32xN_t dir[3];
    float32xN_t dir_len_squared;
    int32xN_t primitive_idx;
};

struct PackedFace {
    float32xN_t min_x;
    float32xN_t face_plane[4];
    float32xN_t edge_plane0[4];
    float32xN_t edge_plane1[4];
    float32xN_t edge_plane2[4];
    int32xN_t primitive_idx;
};

struct FaceData {
    // Plane coefficients of the face plane. Normal is of unit length.
    GEO::vec4 face_plane;
    // Edge plane at index i is the plane that contains the edge opposite to vertex i.
    // Note that edge planes have to be oriented inwards, i.e. the normal is pointing to the
    // inside of the triangle.
    GEO::vec4 clipping_planes[3];

    GEO::vec3 pt_on_plane;
};

struct EdgeData {
    uint32_t start = uint32_t(-1), end = uint32_t(-1);
    GEO::vec4 clipping_planes[4];
    int num_planes = 0;
};

struct BoundingBox {
    GEO::vec3 lower = GEO::vec3{std::numeric_limits<double>::max()};
    GEO::vec3 upper = GEO::vec3{-std::numeric_limits<double>::max()};

    void extend(const GEO::vec3 &pt) {
        lower = {std::min(lower.x, pt.x), std::min(lower.y, pt.y), std::min(lower.z, pt.z)};
        upper = {std::max(upper.x, pt.x), std::max(upper.y, pt.y), std::max(upper.z, pt.z)};
    }

    void extend(const BoundingBox &box) {
        extend(box.lower);
        extend(box.upper);
    }
};

struct Node {
    float32x4_t minCorners[3]; // x, y, z minimum corners for 4 boxes
    float32x4_t maxCorners[3]; // x, y, z maximum corners for 4 boxes
    int32x4_t children;
};

// ============================= SIMD ===============================

#ifdef MANTIS_HAS_NEON

// a*b + c
float32x4_t fma(float32x4_t a, float32x4_t b, float32x4_t c) {
    return vmlaq_f32(c, a, b);
}

float32x4_t min(float32x4_t a, float32x4_t b) {
    return vminq_f32(a, b);
}

float32x4_t max(float32x4_t a, float32x4_t b) {
    return vmaxq_f32(a, b);
}

float32x4_t sub(float32x4_t a, float32x4_t b) {
    return vsubq_f32(a, b);
}

float32x4_t add(float32x4_t a, float32x4_t b) {
    return vaddq_f32(a, b);
}

float32x4_t mul(float32x4_t a, float32x4_t b) {
    return vmulq_f32(a, b);
}

float32x4_t div(float32x4_t a, float32x4_t b) {
    return vdivq_f32(a, b);
}

uint32x4_t leq(float32x4_t a, float32x4_t b) {
    return vcleq_f32(a, b);
}

uint32x4_t geq(float32x4_t a, float32x4_t b) {
    return vcgeq_f32(a, b);
}

uint32x4_t logical_and(uint32x4_t a, uint32x4_t b) {
    return vandq_u32(a, b);
}

int32x4_t select_int(int32x4_t condition, int32x4_t trueValue, int32x4_t falseValue) {
    return vbslq_s32(condition, trueValue, falseValue);
}

float32x4_t select_float(uint32x4_t condition, float32x4_t trueValue, float32x4_t falseValue) {
    return vbslq_f32(condition, trueValue, falseValue);
}

template<int N = SimdWidth>
float32x4_t dupf32(float x) {
    static_assert(N == 4);
    return vdupq_n_f32(x);
}

template<int N = SimdWidth>
int32x4_t dupi32(int32_t x) {
    static_assert(N == 4);
    return vdupq_n_s32(x);
}

#endif

#ifdef MANTIS_HAS_AVX

float32x4_t fma(float32x4_t a, float32x4_t b, float32x4_t c) {
    return _mm_add_ps(_mm_mul_ps(a, b), c); // TODO: check for fma
}

float32x4_t min(float32x4_t a, float32x4_t b) {
    return _mm_min_ps(a, b);
}

float32x4_t max(float32x4_t a, float32x4_t b) {
    return _mm_max_ps(a, b);
}

float32x4_t sub(float32x4_t a, float32x4_t b) {
    return _mm_sub_ps(a, b);
}

float32x4_t add(float32x4_t a, float32x4_t b) {
    return _mm_add_ps(a, b);
}

float32x4_t mul(float32x4_t a, float32x4_t b) {
    return _mm_mul_ps(a, b);
}

float32x4_t div(float32x4_t a, float32x4_t b) {
    return _mm_div_ps(a, b);
}

mask4_t leq(float32x4_t a, float32x4_t b) {
    return _mm_castps_si128(_mm_cmple_ps(a, b)); // Cast result to integer type
}

mask4_t geq(float32x4_t a, float32x4_t b) {
    return _mm_castps_si128(_mm_cmpge_ps(a, b)); // Cast result to integer type
}

mask4_t logical_and(mask4_t a, mask4_t b) {
    return _mm_and_si128(a, b);
}

int32x4_t select_int(int32x4_t condition, int32x4_t trueValue, int32x4_t falseValue) {
    return _mm_blendv_epi8(falseValue, trueValue, condition);
}

float32x4_t select_float(mask4_t condition, float32x4_t trueValue, float32x4_t falseValue) {
    __m128 conditionAsFloat = _mm_castsi128_ps(condition);
    return _mm_blendv_ps(falseValue, trueValue, conditionAsFloat);
}

#ifndef MANTIS_HAS_AVX512
template<int N = SimdWidth>
auto dupf32(float x) {
    static_assert(N == 4);
    return _mm_set1_ps(x);
}

template<int N = SimdWidth>
auto dupi32(int32_t x) {
    static_assert(N == 4);
    return _mm_set1_epi32(x);
}
#endif

#endif

#ifdef MANTIS_HAS_AVX512

float32x16_t fma(float32x16_t a, float32x16_t b, float32x16_t c) {
    return _mm512_fmadd_ps(a, b, c);
}

float32x16_t min(float32x16_t a, float32x16_t b) {
    return _mm512_min_ps(a, b);
}

float32x16_t max(float32x16_t a, float32x16_t b) {
    return _mm512_max_ps(a, b);
}

float32x16_t sub(float32x16_t a, float32x16_t b) {
    return _mm512_sub_ps(a, b);
}

float32x16_t add(float32x16_t a, float32x16_t b) {
    return _mm512_add_ps(a, b);
}

float32x16_t mul(float32x16_t a, float32x16_t b) {
    return _mm512_mul_ps(a, b);
}

float32x16_t div(float32x16_t a, float32x16_t b) {
    return _mm512_div_ps(a, b);
}

mask16_t leq(float32x16_t a, float32x16_t b) {
    return _mm512_cmp_ps_mask(a, b, _CMP_LE_OS);
}

mask16_t geq(float32x16_t a, float32x16_t b) {
    return _mm512_cmp_ps_mask(a, b, _CMP_GE_OS);
}

mask16_t logical_and(mask16_t a, mask16_t b) {
    return _mm512_kand(a, b);
}

int32x16_t select_int(mask16_t condition, int32x16_t trueValue, int32x16_t falseValue) {
    return _mm512_mask_blend_epi32(condition, falseValue, trueValue);
}

float32x16_t select_float(mask16_t condition, float32x16_t trueValue, float32x16_t falseValue) {
    return _mm512_mask_blend_ps(condition, falseValue, trueValue);
}

template<int N = SimdWidth>
auto dupf32(float x) {

    if constexpr(N == 4) {
        return _mm_set1_ps(x);
    } else if constexpr(N == 16) {
        return _mm512_set1_ps(x);
    }
}

template<int N = SimdWidth>
auto dupi32(int32_t x) {
    if constexpr(N == 4) {
        return _mm_set1_epi32(x);
    } else if constexpr(N == 16) {
        return _mm512_set1_epi32(x);
    }
}

void set(float32x16_t &v, size_t i, float x) {
    assert(i < 16);
    auto ptr = (float *) &v;
    ptr[i] = x;
}

void set(int32x16_t &v, size_t i, int x) {
    assert(i < 16);
    auto ptr = (int *) &v;
    ptr[i] = x;
}

float get(const float32x16_t &v, size_t i) {
    assert(i < 16);
    auto ptr = (const float *) &v;
    return ptr[i];
}

int get(const int32x16_t &v, size_t i) {
    assert(i < 16);
    auto ptr = (const int *) &v;
    return ptr[i];
}

#endif

// ============================= SIMD MATH UTILS ===============================

void set(float32x4_t &v, size_t i, float x) {
    auto ptr = (float *) &v;
    ptr[i] = x;
}

void set(int32x4_t &v, size_t i, int x) {
    auto ptr = (int *) &v;
    ptr[i] = x;
}

float get(const float32x4_t &v, size_t i) {
    auto ptr = (const float *) &v;
    return ptr[i];
}

int get(const int32x4_t &v, size_t i) {
    auto ptr = (const int *) &v;
    return ptr[i];
}

template<class T>
T dot(T ax, T ay, T az, T bx, T by, T bz) {
    T result = mul(ax, bx);
    result = fma(ay, by, result);
    result = fma(az, bz, result);
    return result;
}

template<class T>
T length_squared(T x, T y, T z) {
    T result = mul(x, x);
    result = fma(y, y, result);
    result = fma(z, z, result);
    return result;
}

template<class T>
T distance_squared(T ax, T ay, T az, T bx, T by, T bz) {
    T dx = sub(ax, bx);
    T dy = sub(ay, by);
    T dz = sub(az, bz);
    return length_squared(dx, dy, dz);
}

template<class T>
T eval_plane(T px, T py, T pz, T plane_x, T plane_y, T plane_z, T plane_w) {
    T result = mul(px, plane_x);
    result = fma(py, plane_y, result);
    result = fma(pz, plane_z, result);
    return add(result, plane_w);
}

inline float32x4_t p2bbox(const Node &node, const float32x4_t qx, const float32x4_t qy, const float32x4_t qz) {
    // Compute distances in x, y, z directions and clamp them to zero if they are negative
    float32x4_t dx = max(sub(node.minCorners[0], qx), sub(qx, node.maxCorners[0]));
    dx = max(dx, dupf32<4>(0.0f));
    float32x4_t dy = max(sub(node.minCorners[1], qy), sub(qy, node.maxCorners[1]));
    dy = max(dy, dupf32<4>(0.0f));
    float32x4_t dz = max(sub(node.minCorners[2], qz), sub(qz, node.maxCorners[2]));
    dz = max(dz, dupf32<4>(0.0f));
    // Compute squared distances for each box
    float32x4_t squaredDist = length_squared(dx, dy, dz);
    return squaredDist;
}

// ============================= BVH ===============================

struct LeafNode {
    float32xN_t x_coords = dupf32(FLT_MAX);
    float32xN_t y_coords = dupf32(FLT_MAX);
    float32xN_t z_coords = dupf32(FLT_MAX);
    int32xN_t indices = dupi32(-1);
};

#define cmin(a, b) get(distances,a) > get(distances,b) ? b : a
#define cmax(a, b) get(distances,a) > get(distances,b) ? a : b

#define cswap(a, b)  \
    {int tmp = a;    \
    a = cmax(a,b);   \
    b = cmin(tmp, b);}

#define nsort4(a, b, c, d) \
    do                     \
    {                      \
        cswap(a, b);       \
        cswap(c, d);       \
        cswap(a, c);       \
        cswap(b, d);       \
        cswap(b, c);       \
    } while (0)


constexpr static long long NUM_PACKETS = 8;

class Bvh {
public:
    void updateClosestPoint(const float32xN_t &pt_x,
                            const float32xN_t &pt_y,
                            const float32xN_t &pt_z,
                            size_t firstPacket,
                            size_t numPackets,
                            float &bestDistSq,
                            int &bestIdx) const {
        float32xN_t minDist = dupf32(bestDistSq);
        int32xN_t minIdx = dupi32(bestIdx);

        for (size_t i = firstPacket; i < firstPacket + numPackets; ++i) {
            // Compute squared distances for a batch of SimdWidth points
            const auto &leaf = m_leaves[i];
            float32xN_t dx = sub(pt_x, leaf.x_coords);
            float32xN_t dy = sub(pt_y, leaf.y_coords);
            float32xN_t dz = sub(pt_z, leaf.z_coords);
            float32xN_t distSq = length_squared(dx, dy, dz);

            // Comparison mask for distances
            // if distSq >= minDist => keep minDist
            maskN_t keepMinDist = geq(distSq, minDist);
            minDist = min(minDist, distSq);

            // Update the indices
            minIdx = select_int(keepMinDist, minIdx, leaf.indices);
        }

        // Find overall minimum distance and index
        for (int j = 0; j < SimdWidth; ++j) {
            if (get(minDist, j) < bestDistSq) {
                bestDistSq = get(minDist, j);
                bestIdx = get(minIdx, j);
            }
        }
    }

    explicit Bvh(const std::vector<GEO::vec3> &points) {
        // Initialize the original_points vector
        original_points.resize(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            original_points[i] = points[i];
        }

        // Create an index array for all points
        std::vector<int> indices(points.size());
        std::iota(indices.begin(), indices.end(), 0);

        // Build the KD-tree
        BoundingBox box;
        int node_idx = constructTree(indices, 0, indices.size(), 0, box);
        assert(node_idx == 0 || node_idx < 0);
    }

    std::pair<int, float> closestPoint(const GEO::vec3 &q) const {
        constexpr int MAX_STACK_SIZE = 64;
        struct StackNode {
            int nodeIndex;
            float minDistSq;
        };
        StackNode stack[MAX_STACK_SIZE];
        int stackSize = 0;

        float bestDistSq = std::numeric_limits<float>::max();
        int bestIdx = -1;

        // Broadcast query point coordinates to SIMD size
        float32x4_t q_x4 = dupf32<4>(q.x);
        float32x4_t q_y4 = dupf32<4>(q.y);
        float32x4_t q_z4 = dupf32<4>(q.z);

        float32xN_t q_xN = dupf32(q.x);
        float32xN_t q_yN = dupf32(q.y);
        float32xN_t q_zN = dupf32(q.z);

        // Start with the root node
        stack[stackSize++] = {0, 0.0f};
        if (m_nodes.empty()) {
            stack[0].nodeIndex = -1;
        }

        while (stackSize > 0) {
            StackNode current = stack[--stackSize];
            if (current.minDistSq >= bestDistSq) {
                continue;  // Skip nodes that can't possibly contain a closer point
            }
            if (current.nodeIndex < 0) {
                auto [begin, numPackets] = m_leafRange[-(current.nodeIndex + 1)];
                updateClosestPoint(q_xN, q_yN, q_zN, begin, numPackets, bestDistSq, bestIdx);
                continue;
            }

            const Node &node = m_nodes[current.nodeIndex];

            // Compute distances to each child
            float32x4_t distances = p2bbox(node, q_x4, q_y4, q_z4);
            int childIndices[4] = {0, 1, 2, 3};

            // Sort children by distance
            nsort4(childIndices[0], childIndices[1], childIndices[2], childIndices[3]);

            // push children that are internal
            for (int idx: childIndices) {
                int childIdx = get(node.children, idx);
                float childDist = get(distances, idx);
                if (childDist < bestDistSq) {
                    assert(stackSize + 1 < MAX_STACK_SIZE);
                    stack[stackSize++] = {childIdx, childDist};
                }
            }
        }

        return {bestIdx, bestDistSq};
    }

private:
    std::vector<GEO::vec3> original_points;

    std::vector<Node> m_nodes;
    std::vector<LeafNode> m_leaves;
    std::vector<std::pair<int, int>> m_leafRange;

    int constructTree(std::vector<int> &indices, size_t begin, size_t end, size_t depth, BoundingBox &box) {
        if (end - begin <= NUM_PACKETS * SimdWidth) {
            // Update the bounding box for this leaf node
            box = BoundingBox();
            for (size_t i = begin; i < end; ++i) {
                int idx = indices[i];
                box.extend(original_points[idx]);
            }

            int leafIdx = int(m_leafRange.size());
            auto firstLeaf = int(m_leaves.size());
            auto numPackets = int((end - begin + SimdWidth - 1) / SimdWidth);
            m_leafRange.emplace_back(firstLeaf, numPackets);

            for (int i = 0; i < numPackets; ++i) {
                LeafNode leaf{};
                for (size_t j = 0; j < SimdWidth; ++j) {
                    size_t k = i * SimdWidth + j;
                    if (k < end - begin) {
                        set(leaf.x_coords, j, (float) original_points[indices[begin + k]].x);
                        set(leaf.y_coords, j, (float) original_points[indices[begin + k]].y);
                        set(leaf.z_coords, j, (float) original_points[indices[begin + k]].z);
                        set(leaf.indices, j, (int) indices[begin + k]);
                    }
                }
                m_leaves.push_back(leaf);
            }

            // Return negative index to indicate leaf node
            return -(leafIdx + 1);
        }

        Node node{};

        // Split dimensions: Choose different dimensions for each split
        size_t primaryDim = depth % 3;
        size_t secondaryDim = (primaryDim + 1) % 3; // Choose next dimension for secondary split

        // Primary split
        size_t primarySplit = (begin + end) / 2;
        std::nth_element(indices.begin() + (long) begin, indices.begin() + (long) primarySplit,
                         indices.begin() + (long) end,
                         [primaryDim, this](int i1, int i2) {
                             return original_points[i1][primaryDim] < original_points[i2][primaryDim];
                         });

        // Secondary splits
        size_t secondarySplit1 = (begin + primarySplit) / 2;
        size_t secondarySplit2 = (primarySplit + end) / 2;

        std::nth_element(indices.begin() + (long) begin, indices.begin() + (long) secondarySplit1,
                         indices.begin() + (long) primarySplit,
                         [secondaryDim, this](int i1, int i2) {
                             return original_points[i1][secondaryDim] < original_points[i2][secondaryDim];
                         });

        std::nth_element(indices.begin() + (long) primarySplit, indices.begin() + (long) secondarySplit2,
                         indices.begin() + (long) end,
                         [secondaryDim, this](int i1, int i2) {
                             return original_points[i1][secondaryDim] < original_points[i2][secondaryDim];
                         });

        BoundingBox childBoxes[4] = {};

        auto node_idx = int(m_nodes.size());
        m_nodes.emplace_back();

        set(node.children, 0, constructTree(indices, begin, secondarySplit1, depth + 2, childBoxes[0]));
        set(node.children, 1, constructTree(indices, secondarySplit1, primarySplit, depth + 2, childBoxes[1]));
        set(node.children, 2, constructTree(indices, primarySplit, secondarySplit2, depth + 2, childBoxes[2]));
        set(node.children, 3, constructTree(indices, secondarySplit2, end, depth + 2, childBoxes[3]));

        // set bounding boxes of node
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 4; ++j) {
                set(node.minCorners[i], j, (float) childBoxes[j].lower[i]);
                set(node.maxCorners[i], j, (float) childBoxes[j].upper[i]);
            }
        }

        // Combine bounding boxes from children
        box = childBoxes[0];
        for (int i = 1; i < 4; ++i) {
            box.extend(childBoxes[i]);
        }

        m_nodes[node_idx] = node;
        return node_idx;
    }
};

// ============================= UTILS ==================================

template<class F>
void parallel_for(size_t begin, size_t end, F f) {
    // serial implementation
    //for(size_t i = begin; i < end; ++i) {
    //    f(i);
    //}
    //return;

    size_t num_threads = std::thread::hardware_concurrency();

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    size_t chunk_size = (end - begin + num_threads - 1) / num_threads;

    for (size_t i = 0; i < num_threads; ++i) {
        size_t thread_begin = begin + i * chunk_size;
        size_t thread_end = std::min(thread_begin + chunk_size, end);

        threads.emplace_back([thread_begin, thread_end, &f]() {
            for (size_t j = thread_begin; j < thread_end; ++j) {
                f(j);
            }
        });
    }

    for (auto &thread: threads) {
        thread.join();
    }
}

// ============================= GEOMETRY UTILS ===============================

GEO::vec4 to_vec4(GEO::vec3 v, double w) {
    return {v.x, v.y, v.z, w};
}

GEO::vec3 to_vec3(GEO::vec4 v) {
    return {v.x, v.y, v.z};
}

double eval_plane(GEO::vec4 plane, GEO::vec3 p) {
    return plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
}

double distance_to_line_squared(GEO::vec3 p, GEO::vec3 a, GEO::vec3 b) {
    GEO::vec3 ab = b - a;
    GEO::vec3 ap = p - a;
    // Project ap onto ab to find the projected point p'
    GEO::vec3 p_prime = a + ab * (GEO::dot(ap, ab) / GEO::dot(ab, ab));
    return GEO::distance2(p, p_prime);
}

// assumes the plane normal is unit length
double distance_to_plane_squared(GEO::vec3 p, GEO::vec4 plane) {
    assert(std::abs(to_vec3(plane).length() - 1.0) < 1e-8);
    double d = eval_plane(plane, p);
    return d * d;
}

GEO::vec3 project_plane(GEO::vec3 p, const FaceData &face) {
    GEO::vec3 normal = to_vec3(face.face_plane);
    GEO::vec3 pt_on_plane = face.pt_on_plane;
    return p - GEO::dot(normal, p - pt_on_plane) * normal;
}

GEO::vec3 project_line(GEO::vec3 p, GEO::vec3 a, GEO::vec3 b) {
    GEO::vec3 ab = b - a;
    GEO::vec3 ap = p - a;
    return a + ab * (GEO::dot(ap, ab) / GEO::dot(ab, ab));
}

template<class F>
inline GEO::vec3 intersect(GEO::vec3 A, GEO::vec3 B, GEO::vec3 p, F dist_to_element_squared) {
    const double tol = 1e-5;
    double l(0), r(1), m;
    GEO::vec3 cur;
    int T = (int) log2(GEO::length(A - B) / tol);
    if (T <= 0) T = 1;
    while (T--) {
        m = (l + r) / 2;
        cur = (1 - m) * B + m * A;
        if (GEO::distance2(cur, p) > dist_to_element_squared(cur)) r = m;
        else l = m;
    }
    return (1 - l) * B + l * A;
}

// squared distance between a point and the straight line of an edge
inline double dis2_p2e(const GEO::vec3 &p, const EdgeData &e, const std::vector<GEO::vec3> &points) {
    GEO::vec3 dir = GEO::normalize(points[e.end] - points[e.start]);
    return GEO::cross(points[e.start] - p, dir).length2();
}

// squared distance between a point and the plane of a face
inline double dis2_p2f(const GEO::vec3 &p, const FaceData &f) {
    //double d = f.n.dot(points[f.verts.x()] - p);
    double d = GEO::dot(to_vec3(f.face_plane), f.pt_on_plane - p);
    return d * d;
}

// returns true if the vertex corresponding to site_point is intercepting the element,
// otherwise returns false. If an interception is detected, the bounding box of the
// element region clipped with the bisector of the element and the intercepted vertex
// is returned in box.
template<class F>
bool check_and_create_bounding_box(
        const GEO::ConvexCell &C,
        GEO::vec3 site_point,
        F dist_to_element_squared,
        BoundingBox &box) {

    bool is_intercepting = false;
    for (index_t v = 1; v < C.nb_v(); ++v) {
        index_t t = C.vertex_triangle(v);

        // Happens if a clipping plane did not clip anything.
        if (t == VBW::END_OF_LIST) {
            continue;
        }

        int last_region = 0;
        int first_region = 0;
        GEO::vec3 last_pt;
        GEO::vec3 first_pt;
        bool first_pt_set = false;

        do {
            GEO::vec3 pt = C.triangle_point(VBW::ushort(t));

            int region = dist_to_element_squared(pt) < GEO::distance2(pt, site_point) ? -1 : 1;

            if (!first_pt_set) {
                first_pt_set = true;
                first_pt = pt;
                first_region = region;
            }

            if (region == -1) {
                box.extend(pt);
                is_intercepting = true;
            }

            // note that we traverse every edge twice, once from each side, but we only need to compute
            // the intersection point once
            if (last_region == -1 && region == 1) {
                GEO::vec3 intersection = intersect(last_pt, pt, site_point, dist_to_element_squared);
                box.extend(intersection);
            }

            last_pt = pt;
            last_region = region;
            index_t lv = C.triangle_find_vertex(t, v);
            t = C.triangle_adjacent(t, (lv + 1) % 3);
        } while (t != C.vertex_triangle(v));

        // Process the edge connecting the last and first points
        if (last_region == -1 && first_region == 1) {
            GEO::vec3 intersection = intersect(last_pt, first_pt, site_point, dist_to_element_squared);
            box.extend(intersection);
        }
    }

    return is_intercepting;
}

// ============================= DISTANCE TO MESH ===============================

struct Impl {

    Impl(const std::vector<GEO::vec3> &points, const std::vector<std::array<uint32_t, 3>> &triangles,
         double limit_cube_len);

    // for each voronoi cell, check every face of the mesh if the vertex corresponding to the cell
    // "intercepts" the face. This means that after trimming the cell by the face's edge planes, it is
    // contained in the convex region that is closer
    void compute_interception_list();

    Result calc_closest_point(GEO::vec3 q);

    Bvh bvh;

    std::vector<GEO::vec3> points;
    std::vector<std::array<uint32_t, 3>> triangles;

    double limit_cube_len = 0;

    std::vector<EdgeData> edges;
    std::vector<FaceData> faces;

    std::vector<std::vector<PackedEdge>> intercepted_edges_packed;
    std::vector<std::vector<PackedFace>> intercepted_faces_packed;

    std::map<std::pair<index_t, index_t>, size_t> edge_index;

#ifdef DEBUG_MANTIS
    std::map<std::pair<index_t, index_t>, GEO::ConvexCell> vertex_edge_cells;
    std::map<std::pair<index_t, index_t>, GEO::ConvexCell> vertex_face_cells;
    std::map<index_t, GEO::ConvexCell> vor_cells;
    std::map<index_t, GEO::ConvexCell> edge_cells;
    std::map<index_t, GEO::ConvexCell> face_cells;
#endif
};

struct PointEq {
    bool operator()(const GEO::vec3 &a, const GEO::vec3 &b) const {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

struct PointHash {
    size_t operator()(const GEO::vec3 &p) const {
        size_t h = 0;
        for (size_t i = 0; i < 3; ++i) {
            h ^= std::hash<double>{}(p[i]) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

bool check_points(std::vector<GEO::vec3> points) {
    for(auto p : points) {
        if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return false;
        }
    }

    // check for duplicates
    std::unordered_map<GEO::vec3, int, PointHash, PointEq> point_map;
    for(auto p : points) {
        point_map[p]++;
    }

    for(auto [p, count] : point_map) {
        if(count > 1) {
            return false;
        }
    }

    return true;
}

void deduplicate_points(std::vector<GEO::vec3>& points, std::vector<std::array<uint32_t, 3>>& triangles) {
    std::vector<int> vertices(points.size());
    std::iota(vertices.begin(), vertices.end(), 0);

    std::sort(vertices.begin(), vertices.end(), [&points](int a, int b) {
        return std::tie(points[a].x, points[a].y, points[a].z) < std::tie(points[b].x, points[b].y, points[b].z);
    });

    std::vector<GEO::vec3> unique_points;
    unique_points.reserve(points.size());
    std::vector<uint32_t> index_map(points.size());

    auto is_equal = [](const GEO::vec3& a, const GEO::vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };

    for (size_t i = 0; i < vertices.size(); ++i) {
        if (i == 0 || !is_equal(points[vertices[i]], points[vertices[i - 1]])) {
            unique_points.push_back(points[vertices[i]]);
        }
        index_map[vertices[i]] = static_cast<uint32_t>(unique_points.size() - 1);
    }

    if(unique_points.size() == points.size()) {
        return;
    }

    points.swap(unique_points);

    for (auto& triangle : triangles) {
        for (int i = 0; i < 3; ++i) {
            triangle[i] = index_map[triangle[i]];
        }
    }
}

Impl::Impl(const std::vector<GEO::vec3> &points, const std::vector<std::array<index_t, 3>> &triangles,
           double limit_cube_len)
        : points(points), triangles(triangles), bvh(points), limit_cube_len(limit_cube_len) {

    assert(check_points(points));

    static int init_geogram = [] {
        GEO::initialize();
        return 0;
    }();
    (void) init_geogram;

    std::map<std::pair<index_t, index_t>, EdgeData> edge_map;

    for (auto t: triangles) {
        for (int i = 0; i < 3; ++i) {
            index_t v0 = t[i];
            index_t v1 = t[(i + 1) % 3];
            if (v0 > v1) {
                std::swap(v0, v1);
            }
            auto [it, inserted] = edge_map.emplace(std::pair{v0, v1}, EdgeData{v0, v1});
            if (inserted) {
                // populate end planes of edge
                GEO::vec3 start_pt = points[v0];
                GEO::vec3 end_pt = points[v1];

                GEO::vec3 n1 = GEO::normalize(end_pt - start_pt);
                GEO::vec3 n2 = GEO::normalize(start_pt - end_pt);
                auto &ed = it->second;
                ed.clipping_planes[ed.num_planes++] = to_vec4(n1, -GEO::dot(n1, start_pt));
                ed.clipping_planes[ed.num_planes++] = to_vec4(n2, -GEO::dot(n2, end_pt));
            }
        }
    }

    faces.resize(triangles.size());
    for (index_t f = 0; f < faces.size(); ++f) {
        auto [v0, v1, v2] = triangles[f];
        GEO::vec3 p0 = points[v0];
        GEO::vec3 p1 = points[v1];
        GEO::vec3 p2 = points[v2];

        GEO::vec3 n = GEO::normalize(GEO::cross(p1 - p0, p2 - p0));

        GEO::vec3 n0 = GEO::normalize(GEO::cross(p2 - p1, n));
        GEO::vec3 n1 = GEO::normalize(GEO::cross(p0 - p2, n));
        GEO::vec3 n2 = GEO::normalize(GEO::cross(p1 - p0, n));

        GEO::vec4 plane0 = to_vec4(-n0, GEO::dot(n0, p1));
        GEO::vec4 plane1 = to_vec4(-n1, GEO::dot(n1, p2));
        GEO::vec4 plane2 = to_vec4(-n2, GEO::dot(n2, p0));

        faces[f].face_plane = to_vec4(n, -GEO::dot(n, p0));
        faces[f].clipping_planes[0] = plane0;
        faces[f].clipping_planes[1] = plane1;
        faces[f].clipping_planes[2] = plane2;

        faces[f].pt_on_plane = p0;

        auto &ed0 = edge_map[std::minmax(v0, v1)];
        if(ed0.num_planes < 4) {
            ed0.clipping_planes[ed0.num_planes++] = -plane2;
        }

        auto &ed1 = edge_map[std::minmax(v1, v2)];
        if(ed1.num_planes < 4) {
            ed1.clipping_planes[ed1.num_planes++] = -plane0;
        }

        auto &ed2 = edge_map[std::minmax(v2, v0)];
        if(ed2.num_planes < 4) {
            ed2.clipping_planes[ed2.num_planes++] = -plane1;
        }
    }

    edges.reserve(edge_map.size());
    for (const auto &[key, edge]: edge_map) {
        assert(edge.num_planes <= 4);
        edges.push_back(edge);
        edge_index[key] = edges.size() - 1;
    }

    compute_interception_list();
}


// for each voronoi cell, check every face of the mesh if the vertex corresponding to the cell
// "intercepts" the face. This means that after trimming the cell by the face's edge planes, it is
// contained in the convex region that is closer
void Impl::compute_interception_list() {
    const index_t nb_points = points.size();
    const index_t nb_faces = triangles.size();
    const index_t nb_edges = edges.size();

    double l = limit_cube_len * 2;
    auto copy = points;
    copy.emplace_back(l, l, l);
    copy.emplace_back(-l, l, l);
    copy.emplace_back(l, -l, l);
    copy.emplace_back(l, l, -l);
    copy.emplace_back(-l, -l, l);
    copy.emplace_back(-l, l, -l);
    copy.emplace_back(l, -l, -l);
    copy.emplace_back(-l, -l, -l);

    assert(check_points(copy));

    GEO::SmartPointer<GEO::PeriodicDelaunay3d> delaunay = new GEO::PeriodicDelaunay3d(false, 1.0);
    delaunay->set_keeps_infinite(true);
    delaunay->set_stores_neighbors(true);
    delaunay->set_vertices(copy.size(), (double *) copy.data());
    delaunay->compute();

    GEO::PeriodicDelaunay3d::IncidentTetrahedra W;

#ifdef DEBUG_MANTIS
    for (index_t v = 0; v < nb_points; ++v) {
            GEO::vec3 site_p = points[v];
            delaunay->copy_Laguerre_cell_from_Delaunay(v, C, W);
            C.compute_geometry();
            vor_cells[v] = C;
        }

        for (index_t e = 0; e < nb_edges; ++e) {
            GEO::ConvexCell clipped_cell;
            clipped_cell.init_with_box(-l, -l, -l, l, l, l);
            for (const GEO::vec4 &clipping_plane: edges[e].clipping_planes) {
                clipped_cell.clip_by_plane(clipping_plane);
            }
            clipped_cell.compute_geometry();
            edge_cells[e] = clipped_cell;
        }

        for (index_t f = 0; f < nb_faces; ++f) {
            GEO::ConvexCell clipped_cell;
            clipped_cell.init_with_box(-l, -l, -l, l, l, l);
            for (const GEO::vec4 &clipping_plane: faces[f].clipping_planes) {
                clipped_cell.clip_by_plane(clipping_plane);
            }
            clipped_cell.compute_geometry();
            face_cells[f] = clipped_cell;
        }
#endif

    std::vector<GEO::ConvexCell> voronoi_cells(nb_points);

    for (index_t v = 0; v < nb_points; ++v) {
        delaunay->copy_Laguerre_cell_from_Delaunay(v, voronoi_cells[v], W);
        voronoi_cells[v].compute_geometry();
    }

    std::vector<std::vector<index_t>> face_vertex(nb_faces);
    std::vector<std::vector<BoundingBox>> face_vertex_bb(nb_faces);

    auto handle_face = [this, &voronoi_cells, delaunay, nb_points, &face_vertex, &face_vertex_bb](index_t f) {
        auto dist_squared = [plane = faces[f].face_plane](GEO::vec3 p) {
            return distance_to_plane_squared(p, plane);
        };

        std::unordered_set < index_t > visited = {triangles[f][0], triangles[f][1], triangles[f][2]};
        std::queue<index_t> queue;
        queue.push(triangles[f][0]);
        queue.push(triangles[f][1]);
        queue.push(triangles[f][2]);

        while (!queue.empty()) {
            index_t v = queue.front();
            queue.pop();

            //delaunay->copy_Laguerre_cell_from_Delaunay(v, C, W);
            GEO::ConvexCell C = voronoi_cells[v];
            for (const GEO::vec4 &clipping_plane: faces[f].clipping_planes) {
                C.clip_by_plane(clipping_plane);
            }
            if (C.empty()) {
                continue;
            }
            C.compute_geometry();
            BoundingBox box;
            if (check_and_create_bounding_box(C, points[v], dist_squared, box)) {
                face_vertex[f].push_back(v);
                face_vertex_bb[f].push_back(box);
#ifdef DEBUG_MANTIS
                vertex_face_cells[{v, f}] = C;
#endif
            } else {
                continue;
            }

            GEO::vector<index_t> neighbors;
            delaunay->get_neighbors(v, neighbors);
            for (index_t n: neighbors) {
                if (n < nb_points && !visited.count(n)) {
                    visited.insert(n);
                    queue.push(n);
                }
            }
        }
    };

    parallel_for(0, nb_faces, handle_face);

    std::vector<std::vector<index_t>> edge_vertex(nb_edges);
    std::vector<std::vector<BoundingBox>> edge_vertex_bb(nb_edges);

    auto handle_edge = [this, &voronoi_cells, delaunay, nb_points, &edge_vertex, &edge_vertex_bb](index_t e) {
        GEO::vec3 l0 = points[edges[e].start];
        GEO::vec3 l1 = points[edges[e].end];
        auto dist_squared = [l0, l1](GEO::vec3 p) {
            return distance_to_line_squared(p, l0, l1);
        };

        std::unordered_set < index_t > visited = {edges[e].start, edges[e].end};
        std::queue<index_t> queue;
        queue.push(edges[e].start);
        queue.push(edges[e].end);

        while (!queue.empty()) {
            index_t v = queue.front();
            queue.pop();

            //delaunay->copy_Laguerre_cell_from_Delaunay(v, C, W);
            GEO::ConvexCell C = voronoi_cells[v];
            for (size_t i = 0; i < edges[e].num_planes; ++i) {
                C.clip_by_plane(edges[e].clipping_planes[i]);
            }
            if (C.empty()) {
                continue;
            }
            C.compute_geometry();
            BoundingBox box;
            if (check_and_create_bounding_box(C, points[v], dist_squared, box)) {
                edge_vertex[e].push_back(v);
                edge_vertex_bb[e].push_back(box);
#ifdef DEBUG_MANTIS
                vertex_edge_cells[{v, e}] = C;
#endif
            } else {
                continue;
            }

            GEO::vector<index_t> neighbors;
            delaunay->get_neighbors(v, neighbors);
            for (index_t n: neighbors) {
                if (n < nb_points && !visited.count(n)) {
                    visited.insert(n);
                    queue.push(n);
                }
            }
        }
    };

    parallel_for(0, nb_edges, handle_edge);

    std::vector<std::vector<index_t>> intercepted_edges(nb_points);
    std::vector<std::vector<BoundingBox>> intercepted_edges_bb(nb_points);
    std::vector<std::vector<index_t>> intercepted_faces(nb_points);
    std::vector<std::vector<BoundingBox>> intercepted_faces_bb(nb_points);

    // transpose edge_vertex and face_vertex arrays
    for (index_t e = 0; e < nb_edges; ++e) {
        for (size_t i = 0; i < edge_vertex[e].size(); ++i) {
            index_t v = edge_vertex[e][i];
            intercepted_edges[v].push_back(e);
            intercepted_edges_bb[v].push_back(edge_vertex_bb[e][i]);
        }
    }
    for (index_t f = 0; f < nb_faces; ++f) {
        for (size_t i = 0; i < face_vertex[f].size(); ++i) {
            index_t v = face_vertex[f][i];
            intercepted_faces[v].push_back(f);
            intercepted_faces_bb[v].push_back(face_vertex_bb[f][i]);
        }
    }

    intercepted_edges_packed.resize(nb_points);
    intercepted_faces_packed.resize(nb_points);

    // Pack data into simd friendly data structures
    std::vector<int> order;
    for (index_t v = 0; v < nb_points; ++v) {
        // first reorder edges
        order.resize(intercepted_edges[v].size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](index_t a, index_t b) {
            return intercepted_edges_bb[v][a].lower.x < intercepted_edges_bb[v][b].lower.x;
        });

        std::vector<index_t> new_intercepted_edges(intercepted_edges[v].size());
        std::vector<BoundingBox> new_intercepted_edges_bb(intercepted_edges_bb[v].size());
        for (index_t i = 0; i < order.size(); ++i) {
            new_intercepted_edges[i] = intercepted_edges[v][order[i]];
            new_intercepted_edges_bb[i] = intercepted_edges_bb[v][order[i]];
        }

        intercepted_edges[v] = std::move(new_intercepted_edges);
        intercepted_edges_bb[v] = std::move(new_intercepted_edges_bb);

        // round up number of edge batches
        size_t num_edge_packed = (intercepted_edges[v].size() + SimdWidth - 1) / SimdWidth;
        intercepted_edges_packed[v].resize(num_edge_packed);

        for (size_t i = 0; i < num_edge_packed; ++i) {
            PackedEdge packed{};
            for (size_t j = 0; j < SimdWidth; ++j) {
                if (i * SimdWidth + j < intercepted_edges[v].size()) {
                    index_t e = intercepted_edges[v][i * SimdWidth + j];
                    set(packed.min_x, j, (float) intercepted_edges_bb[v][i * SimdWidth + j].lower.x);
                    for (size_t d = 0; d < 3; ++d) {
                        set(packed.start[d], j, (float) points[edges[e].start][d]);
                        set(packed.dir[d], j, float(points[edges[e].end][d] - points[edges[e].start][d]));
                    }
                    set(packed.dir_len_squared, j, float(GEO::distance2(points[edges[e].end], points[edges[e].start])));
                    set(packed.primitive_idx, j, int(e + nb_points));
                } else {
                    // duplicate last edge
                    assert(j > 0);
                    set(packed.min_x, j, get(packed.min_x, j - 1));
                    for (size_t d = 0; d < 3; ++d) {
                        set(packed.start[d], j, get(packed.start[d], j - 1));
                        set(packed.dir[d], j, get(packed.dir[d], j - 1));
                    }
                    set(packed.dir_len_squared, j, get(packed.dir_len_squared, j - 1));
                    set(packed.primitive_idx, j, get(packed.primitive_idx, j - 1));
                }
            }
            intercepted_edges_packed[v][i] = packed;
        }

        // then reorder faces
        order.resize(intercepted_faces[v].size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](index_t a, index_t b) {
            return intercepted_faces_bb[v][a].lower.x < intercepted_faces_bb[v][b].lower.x;
        });
        std::vector<index_t> new_intercepted_faces(intercepted_faces[v].size());
        std::vector<BoundingBox> new_intercepted_faces_bb(intercepted_faces_bb[v].size());
        for (index_t i = 0; i < order.size(); ++i) {
            new_intercepted_faces[i] = intercepted_faces[v][order[i]];
            new_intercepted_faces_bb[i] = intercepted_faces_bb[v][order[i]];
        }

        intercepted_faces[v] = std::move(new_intercepted_faces);
        intercepted_faces_bb[v] = std::move(new_intercepted_faces_bb);

        // round up nb of face batches
        size_t num_face_packed = (intercepted_faces[v].size() + SimdWidth - 1) / SimdWidth;
        intercepted_faces_packed[v].resize(num_face_packed);
        intercepted_faces_bb[v].resize(num_face_packed);

        for (size_t i = 0; i < num_face_packed; ++i) {
            PackedFace packed{};
            for (size_t j = 0; j < SimdWidth; ++j) {
                if (i * SimdWidth + j < intercepted_faces[v].size()) {
                    index_t f = intercepted_faces[v][i * SimdWidth + j];
                    set(packed.min_x, j, (float) intercepted_faces_bb[v][i * SimdWidth + j].lower.x);
                    for (size_t d = 0; d < 4; ++d) {
                        set(packed.face_plane[d], j, (float) faces[f].face_plane[d]);
                        set(packed.edge_plane0[d], j, (float) faces[f].clipping_planes[0][d]);
                        set(packed.edge_plane1[d], j, (float) faces[f].clipping_planes[1][d]);
                        set(packed.edge_plane2[d], j, (float) faces[f].clipping_planes[2][d]);
                    }
                    set(packed.primitive_idx, j, int(f + nb_points + nb_edges));
                } else {
                    // duplicate last face
                    assert(j > 0);
                    set(packed.min_x, j, get(packed.min_x, j - 1));
                    for (size_t d = 0; d < 4; ++d) {
                        set(packed.face_plane[d], j, get(packed.face_plane[d], j - 1));
                        set(packed.edge_plane0[d], j, get(packed.edge_plane0[d], j - 1));
                        set(packed.edge_plane1[d], j, get(packed.edge_plane1[d], j - 1));
                        set(packed.edge_plane2[d], j, get(packed.edge_plane2[d], j - 1));
                    }
                }
            }
            intercepted_faces_packed[v][i] = packed;
        }
    }
}

Result Impl::calc_closest_point(GEO::vec3 q) {
    auto [v, v_dist2] = bvh.closestPoint(q);

    float32xN_t qx = dupf32((float) q.x);
    float32xN_t qy = dupf32((float) q.y);
    float32xN_t qz = dupf32((float) q.z);

    float32xN_t best_d2 = dupf32(v_dist2);
    int32xN_t best_idx = dupi32(v);

    const auto &v_edges = intercepted_edges_packed[v];
    for (const PackedEdge &pack: v_edges) {
        if (q.x < get(pack.min_x, 0)) {
            break;
        }

        float32xN_t apx = sub(qx, pack.start[0]);
        float32xN_t apy = sub(qy, pack.start[1]);
        float32xN_t apz = sub(qz, pack.start[2]);

        float32xN_t t = div(dot(apx, apy, apz, pack.dir[0], pack.dir[1], pack.dir[2]), pack.dir_len_squared);

        // the result is only valid if t is in [0, 1]
        maskN_t mask = logical_and(leq(dupf32(0.0f), t), leq(t, dupf32(1.0f)));

        // project onto segment
        float32xN_t projectedx = fma(t, pack.dir[0], pack.start[0]);
        float32xN_t projectedy = fma(t, pack.dir[1], pack.start[1]);
        float32xN_t projectedz = fma(t, pack.dir[2], pack.start[2]);

        float32xN_t d2_line = distance_squared(qx, qy, qz, projectedx, projectedy, projectedz);

        mask = logical_and(mask, leq(d2_line, best_d2));
        best_d2 = select_float(mask, d2_line, best_d2);
        best_idx = select_int(mask, pack.primitive_idx, best_idx);
    }

    const auto &v_faces = intercepted_faces_packed[v];
    for (const PackedFace &pack: v_faces) {
        if (q.x < get(pack.min_x, 0)) {
            break;
        }

        // point is inside face region if it is on the positive side of all three planes
        float32xN_t s0 = eval_plane(qx, qy, qz, pack.edge_plane0[0], pack.edge_plane0[1], pack.edge_plane0[2],
                                    pack.edge_plane0[3]);
        float32xN_t s1 = eval_plane(qx, qy, qz, pack.edge_plane1[0], pack.edge_plane1[1], pack.edge_plane1[2],
                                    pack.edge_plane1[3]);
        float32xN_t s2 = eval_plane(qx, qy, qz, pack.edge_plane2[0], pack.edge_plane2[1], pack.edge_plane2[2],
                                    pack.edge_plane2[3]);

        maskN_t mask = logical_and(logical_and(leq(dupf32(0.0f), s0), leq(dupf32(0.0f), s1)),
                                      leq(dupf32(0.0f), s2));

        float32xN_t d2 = eval_plane(qx, qy, qz, pack.face_plane[0], pack.face_plane[1], pack.face_plane[2],
                                    pack.face_plane[3]);
        d2 = mul(d2, d2);

        mask = logical_and(mask, leq(d2, best_d2));
        best_d2 = select_float(mask, d2, best_d2);
        best_idx = select_int(mask, pack.primitive_idx, best_idx);
    }

    Result result{get(best_d2, 0), get(best_idx, 0)};

    // Find overall minimum distance and index
    for (int j = 1; j < SimdWidth; ++j) {
        if (get(best_d2, j) < result.distance_squared) {
            result.distance_squared = get(best_d2, j);
            result.primitive_index = get(best_idx, j);
        }
    }

    GEO::vec3 cp;
    if (result.primitive_index < points.size()) {
        cp = points[result.primitive_index];
        result.type = PrimitiveType::Vertex;
    } else if (result.primitive_index < points.size() + edges.size()) {
        int offset = (int) points.size();
        const auto &e = edges[result.primitive_index - offset];
        cp = project_line(q, points[e.start], points[e.end]);
        result.primitive_index -= offset;
        result.type = PrimitiveType::Edge;
    } else {
        int offset = (int) points.size() + (int) edges.size();
        auto f = faces[result.primitive_index - offset];
        cp = project_plane(q, f);
        result.primitive_index -= offset;
        result.type = PrimitiveType::Face;
    }
    result.closest_point[0] = (float) cp.x;
    result.closest_point[1] = (float) cp.y;
    result.closest_point[2] = (float) cp.z;

    return result;
}

AccelerationStructure::AccelerationStructure(const float *points, size_t num_points, const uint32_t *indices,
                                             size_t num_faces,
                                             float limit_cube_len) {
    std::vector<GEO::vec3> points_vec(num_points);
    for (size_t i = 0; i < num_points; ++i) {
        points_vec[i] = {points[3 * i], points[3 * i + 1], points[3 * i + 2]};
    }
    std::vector<std::array<uint32_t, 3>> faces_vec(num_faces);
    for (size_t i = 0; i < num_faces; ++i) {
        faces_vec[i] = {indices[3 * i], indices[3 * i + 1], indices[3 * i + 2]};
    }
    deduplicate_points(points_vec, faces_vec);
    impl = new Impl(points_vec, faces_vec, limit_cube_len);
}

AccelerationStructure::AccelerationStructure(const std::vector<std::array<float, 3>> &points,
                                             const std::vector<std::array<uint32_t, 3>> &triangles,
                                             float limit_cube_len) :
        AccelerationStructure((const float *) points.data(), points.size(), (const uint32_t *) triangles.data(),
                              triangles.size(), limit_cube_len) {}

AccelerationStructure::AccelerationStructure(AccelerationStructure &&other) noexcept {
    impl = other.impl;
    other.impl = nullptr;
}

AccelerationStructure &AccelerationStructure::operator=(AccelerationStructure &&other) noexcept {
    if (this != &other) {
        delete impl;
        impl = other.impl;
        other.impl = nullptr;
    }
    return *this;
}

Result AccelerationStructure::calc_closest_point(float x, float y, float z) const {
    return impl->calc_closest_point({x, y, z});
}

Result AccelerationStructure::calc_closest_point(std::array<float, 3> q) const {
    return impl->calc_closest_point({q[0], q[1], q[2]});
}

std::vector<std::array<uint32_t, 3>> AccelerationStructure::get_face_edges() const {
    std::vector<std::array<uint32_t, 3>> result(num_faces());
    std::vector<int> current_index(num_faces(), 0);

    for (size_t i = 0; i < num_faces(); ++i) {
        const auto &f = impl->faces[i];
        for (size_t j = 0; j < 3; ++j) {
            auto v0 = impl->triangles[i][j];
            auto v1 = impl->triangles[i][(j + 1) % 3];
            auto it = impl->edge_index.find(std::minmax(v0, v1));
            assert(it != impl->edge_index.end());
            auto idx = it->second;
            result[i][current_index[i]++] = idx;
        }
    }
    return result;
}

std::vector<std::pair<uint32_t, uint32_t>> AccelerationStructure::get_edge_vertices() const {
    std::vector<std::pair<uint32_t, uint32_t>> result(num_edges());
    for (size_t i = 0; i < num_edges(); ++i) {
        const auto &e = impl->edges[i];
        result[i] = {e.start, e.end};
    }
    return result;
}

std::vector<std::array<uint32_t, 3>> AccelerationStructure::get_faces() const {
    return impl->triangles;
}

std::vector<std::array<float, 3>> AccelerationStructure::get_positions() const {
    std::vector<std::array<float, 3>> result(num_vertices());
    for (size_t i = 0; i < num_vertices(); ++i) {
        const auto &p = impl->points[i];
        result[i] = {float(p.x), float(p.y), float(p.z)};
    }
    return result;
}

std::pair<uint32_t, uint32_t> AccelerationStructure::get_edge(size_t index) const {
    const auto &e = impl->edges[index];
    return {e.start, e.end};
}

size_t AccelerationStructure::num_edges() const {
    return impl->edges.size();
}

size_t AccelerationStructure::num_faces() const {
    return impl->triangles.size();
}

size_t AccelerationStructure::num_vertices() const {
    return impl->points.size();
}

AccelerationStructure::~AccelerationStructure() {
    delete impl;
}

}