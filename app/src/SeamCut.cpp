#include "SeamCut.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
// Every edge keeps a floor cost so the graph stays connected on a perfectly
// flat image. Without it the flow is zero everywhere, nothing is reachable from
// the source, and the partition degenerates to "no cut chosen".
constexpr int kMaxGridSide = 192;
constexpr double kEdgeFloor = 1e-3;
constexpr double kFlowEpsilon = 1e-9;

int wrap(int value, int size)
{
    const int m = value % size;
    return m < 0 ? m + size : m;
}
} // namespace

SeamMaxFlow::SeamMaxFlow(int nodes)
    : m_nodes(nodes)
    , m_adjacency(static_cast<size_t>(nodes))
{
}

void SeamMaxFlow::addEdge(int u, int v, double capacity)
{
    const int forward = static_cast<int>(m_edges.size());
    const int backward = forward + 1;
    m_edges.push_back({v, capacity, backward});
    m_edges.push_back({u, capacity, forward});
    m_adjacency[static_cast<size_t>(u)].push_back(forward);
    m_adjacency[static_cast<size_t>(v)].push_back(backward);
}

double SeamMaxFlow::solve(int source, int sink)
{
    const int n = m_nodes;
    // Shortest augmenting path (ISAP), not push-relabel.
    //
    // Push-relabel starts by saturating every edge out of the source, so the
    // graph is flooded with far more excess than can ever reach the sink; the
    // surplus then has to be walked back to the source one relabel at a time.
    // On a 256x256 seam this measured 670 million relabels and 100 seconds for
    // a flow of 25. An augmenting-path method only ever injects flow along a
    // complete path, so nothing has to be returned.
    std::vector<int> distance(static_cast<size_t>(n), n);
    std::vector<int> gap(static_cast<size_t>(n) + 2, 0);
    std::vector<int> current(static_cast<size_t>(n), 0);
    std::vector<int> parent(static_cast<size_t>(n), -1);
    std::vector<int> parentEdge(static_cast<size_t>(n), -1);

    // Initial labelling: distance to the sink in the residual graph.
    {
        std::vector<int> queue;
        queue.reserve(static_cast<size_t>(n));
        distance[static_cast<size_t>(sink)] = 0;
        queue.push_back(sink);
        for (size_t head = 0; head < queue.size(); ++head) {
            const int v = queue[head];
            const int next = distance[static_cast<size_t>(v)] + 1;
            for (const int index : m_adjacency[static_cast<size_t>(v)]) {
                const Edge& edge = m_edges[static_cast<size_t>(index)];
                const int to = edge.to;
                if (distance[static_cast<size_t>(to)] == n
                    && m_edges[static_cast<size_t>(edge.reverse)].capacity > kFlowEpsilon) {
                    distance[static_cast<size_t>(to)] = next;
                    queue.push_back(to);
                }
            }
        }
    }
    for (int v = 0; v < n; ++v) {
        ++gap[static_cast<size_t>(distance[static_cast<size_t>(v)])];
    }

    double flow = 0.0;
    int node = source;
    while (distance[static_cast<size_t>(source)] < n) {
        if (node == sink) {
            // Walk back for the bottleneck, then push it through.
            double amount = std::numeric_limits<double>::max();
            for (int v = sink; v != source; v = parent[static_cast<size_t>(v)]) {
                amount = std::min(amount, m_edges[static_cast<size_t>(parentEdge[static_cast<size_t>(v)])].capacity);
            }
            for (int v = sink; v != source; v = parent[static_cast<size_t>(v)]) {
                Edge& edge = m_edges[static_cast<size_t>(parentEdge[static_cast<size_t>(v)])];
                edge.capacity -= amount;
                m_edges[static_cast<size_t>(edge.reverse)].capacity += amount;
            }
            flow += amount;
            node = source;
        }

        bool advanced = false;
        std::vector<int>& adjacency = m_adjacency[static_cast<size_t>(node)];
        int& arc = current[static_cast<size_t>(node)];
        for (; arc < static_cast<int>(adjacency.size()); ++arc) {
            const int index = adjacency[static_cast<size_t>(arc)];
            const Edge& edge = m_edges[static_cast<size_t>(index)];
            if (edge.capacity > kFlowEpsilon
                && distance[static_cast<size_t>(node)] == distance[static_cast<size_t>(edge.to)] + 1) {
                parent[static_cast<size_t>(edge.to)] = node;
                parentEdge[static_cast<size_t>(edge.to)] = index;
                node = edge.to;
                advanced = true;
                break;
            }
        }
        if (advanced) {
            continue;
        }

        // Nothing admissible from here: raise this vertex's label to one above
        // its best remaining neighbour and back off one step.
        const int oldDistance = distance[static_cast<size_t>(node)];
        --gap[static_cast<size_t>(oldDistance)];
        if (gap[static_cast<size_t>(oldDistance)] == 0) {
            // No vertex is left at that distance, so the source can never reach
            // the sink again. This is what keeps the loop from running on.
            break;
        }
        int best = n;
        for (const int index : adjacency) {
            const Edge& edge = m_edges[static_cast<size_t>(index)];
            if (edge.capacity > kFlowEpsilon) {
                best = std::min(best, distance[static_cast<size_t>(edge.to)]);
            }
        }
        distance[static_cast<size_t>(node)] = (best >= n) ? n : best + 1;
        ++gap[static_cast<size_t>(distance[static_cast<size_t>(node)])];
        current[static_cast<size_t>(node)] = 0;
        if (node != source) {
            node = parent[static_cast<size_t>(node)];
        }
    }

    // The minimum cut is the set of vertices still reachable from the source in
    // the residual graph.
    m_sourceSide.assign(static_cast<size_t>(n), 0);
    std::vector<int> stack{source};
    m_sourceSide[static_cast<size_t>(source)] = 1;
    while (!stack.empty()) {
        const int v = stack.back();
        stack.pop_back();
        for (const int index : m_adjacency[static_cast<size_t>(v)]) {
            const Edge& edge = m_edges[static_cast<size_t>(index)];
            if (edge.capacity > kFlowEpsilon && !m_sourceSide[static_cast<size_t>(edge.to)]) {
                m_sourceSide[static_cast<size_t>(edge.to)] = 1;
                stack.push_back(edge.to);
            }
        }
    }
    return flow;
}

bool SeamMaxFlow::sourceSide(int node) const
{
    return m_sourceSide[static_cast<size_t>(node)] != 0;
}

QImage buildSeamlessCutMask(const QImage& image, SeamAxis axis)
{
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const int width = source.width();
    const int height = source.height();
    if (width < 8 || height < 8) {
        return QImage();
    }

    // The cut only has to be placed to within a couple of pixels - the blend
    // feathers it afterwards - so it is computed on a coarser grid. Flow cost
    // grows with the square of the grid, and this runs on every preview.
    int step = 1;
    while ((std::max(width, height) + step - 1) / step > kMaxGridSide) {
        step *= 2;
    }
    const int gridWidth = width / step;
    const int gridHeight = height / step;
    if (gridWidth < 6 || gridHeight < 6) {
        return QImage();
    }

    // --- The two copies, box-downsampled -----------------------------------
    // Costs are compared at the grid resolution, so downsampling first is not
    // just a speed trick: it is the scale the decision is actually made at.
    const bool horizontal = axis == SeamAxis::Horizontal;
    const int halfX = horizontal ? width / 2 : 0;
    const int halfY = horizontal ? 0 : height / 2;
    const size_t cellCount = static_cast<size_t>(gridWidth) * gridHeight;
    std::vector<float> originalAlpha(cellCount * 4, 0.0f);
    std::vector<float> shiftedAlpha(cellCount * 4, 0.0f);

    const double inverseArea = 1.0 / (static_cast<double>(step) * step);
    for (int gy = 0; gy < gridHeight; ++gy) {
        for (int gx = 0; gx < gridWidth; ++gx) {
            double original[4] = {0.0, 0.0, 0.0, 0.0};
            double shifted[4] = {0.0, 0.0, 0.0, 0.0};
            for (int dy = 0; dy < step; ++dy) {
                const int y = gy * step + dy;
                const QRgb* originalLine = reinterpret_cast<const QRgb*>(source.constScanLine(y));
                const QRgb* shiftedLine = reinterpret_cast<const QRgb*>(
                    source.constScanLine(wrap(y + halfY, height)));
                for (int dx = 0; dx < step; ++dx) {
                    const int x = gx * step + dx;
                    const QRgb a = originalLine[x];
                    original[0] += qRed(a);
                    original[1] += qGreen(a);
                    original[2] += qBlue(a);
                    original[3] += qAlpha(a);
                    const QRgb b = shiftedLine[wrap(x + halfX, width)];
                    shifted[0] += qRed(b);
                    shifted[1] += qGreen(b);
                    shifted[2] += qBlue(b);
                    shifted[3] += qAlpha(b);
                }
            }
            const size_t cell = (static_cast<size_t>(gy) * gridWidth + gx) * 4;
            for (int c = 0; c < 4; ++c) {
                originalAlpha[cell + c] = static_cast<float>(original[c] * inverseArea);
                shiftedAlpha[cell + c] = static_cast<float>(shifted[c] * inverseArea);
            }
        }
    }

    // Cost of the switch happening across the edge between p and q: the output
    // goes from "p of the source" to "q of the shifted copy", so the step the
    // eye sees is the difference between exactly those two.
    auto mismatch = [&](int p, int q) {
        double total = 0.0;
        for (int c = 0; c < 4; ++c) {
            total += std::abs(originalAlpha[static_cast<size_t>(p) * 4 + c]
                              - shiftedAlpha[static_cast<size_t>(q) * 4 + c]);
        }
        return total / 4.0 / 255.0;
    };

    const int gridNodes = gridWidth * gridHeight;
    const int sourceNode = gridNodes;
    const int sinkNode = gridNodes + 1;
    SeamMaxFlow graph(gridNodes + 2);
    double gridCapacity = 0.0;
    auto link = [&](int p, int q) {
        const double cost = 0.5 * (mismatch(p, q) + mismatch(q, p)) + kEdgeFloor;
        graph.addEdge(p, q, cost);
        gridCapacity += cost;
    };
    for (int gy = 0; gy < gridHeight - 1; ++gy) {
        for (int gx = 0; gx < gridWidth - 1; ++gx) {
            const int p = gy * gridWidth + gx;
            link(p, p + 1);
            link(p, p + gridWidth);
        }
    }
    // The last column and row still need their edges to the inward neighbour on
    // the other axis.
    for (int gy = 0; gy < gridHeight - 1; ++gy) {
        const int p = gy * gridWidth + (gridWidth - 1);
        link(p, p + gridWidth);
    }
    for (int gx = 0; gx < gridWidth - 1; ++gx) {
        const int p = (gridHeight - 1) * gridWidth + gx;
        link(p, p + 1);
    }

    // Terminal capacity only has to exceed any cut the grid can offer.
    const double terminalCapacity = 2.0 * gridCapacity + 1.0;

    // Along the axis being tiled: both edge strips are pinned to the shifted
    // copy, a band around the middle is pinned to the source, and the cut is
    // free in the two channels between them.
    //
    // Everything here is one-dimensional. Pinning a *ring* instead - which is
    // what this used to do - cannot work once the copy is shifted on both axes:
    // the ring has to cross the shifted copy's own discontinuity, so the seam
    // simply moves from the tile border into the middle of the tile. Measured
    // on a texture with a lighting gradient, that came out as a 2x2
    // checkerboard of the two copies with hard edges.
    //
    // The outer pins are not decoration: the coarse bands are feathered across
    // many pixels, and if the shifted copy does not cover the tile edge by
    // several coarse pixels, the overall colour at one edge comes from the
    // source and at the other from the shifted copy - a brightness step at
    // every tile boundary, exactly the seam the pass exists to remove.
    //
    // The middle pin covers the shifted copy's own wrap line, and it also stops
    // the opposite failure: with no source-side terminal at all, the cheapest
    // cut is "shifted copy everywhere", which is not a tiling - it is the
    // source image moved.
    const int along = horizontal ? gridWidth : gridHeight;
    const int bandOuter = std::max(2, along / 12);
    const int innerHalf = std::max(2, along / 8);
    const int centre = along / 2;
    if (centre - innerHalf <= bandOuter || centre + innerHalf >= along - 1 - bandOuter) {
        // Too narrow for a free channel on both sides of the middle. Clamping
        // here would pin every cell to one copy and produce exactly the mask
        // this check exists to refuse; the caller falls back instead.
        return QImage();
    }
    for (int gy = 0; gy < gridHeight; ++gy) {
        for (int gx = 0; gx < gridWidth; ++gx) {
            const int a = horizontal ? gx : gy;
            if (a < bandOuter || a >= along - bandOuter) {
                graph.addEdge(gy * gridWidth + gx, sinkNode, terminalCapacity);
            } else if (std::min(a - centre, centre - a) <= innerHalf) {
                graph.addEdge(sourceNode, gy * gridWidth + gx, terminalCapacity);
            }
        }
    }

    graph.solve(sourceNode, sinkNode);

    QImage mask(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        uchar* line = mask.scanLine(y);
        const int gy = std::min(gridHeight - 1, y / step);
        for (int x = 0; x < width; ++x) {
            const int gx = std::min(gridWidth - 1, x / step);
            line[x] = graph.sourceSide(gy * gridWidth + gx) ? 255 : 0;
        }
    }
    return mask;
}
