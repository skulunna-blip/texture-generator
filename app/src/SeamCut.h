#pragma once

#include <QImage>

#include <vector>

// Maximum flow (shortest augmenting path, gap heuristic) on an undirected graph.
//
// Exposed rather than file-local because the regression test checks it against
// brute force over every labelling of small graphs. The seam quality rests
// entirely on this being a true minimum cut, and a plausible-looking wrong cut
// looks exactly like a right one in the output - there is nothing in the image
// that would give it away.
class SeamMaxFlow
{
public:
    explicit SeamMaxFlow(int nodes);

    // Adds an undirected edge: capacity is available in both directions.
    void addEdge(int u, int v, double capacity);

    // Runs the flow. Afterwards sourceSide() reports the minimum cut, and the
    // return value is the flow through it (equal to the cut's capacity).
    double solve(int source, int sink);

    bool sourceSide(int node) const;
    int nodeCount() const { return m_nodes; }

private:
    struct Edge
    {
        int to;
        double capacity;
        int reverse;
    };

    int m_nodes;
    std::vector<Edge> m_edges;
    std::vector<std::vector<int>> m_adjacency;
    std::vector<char> m_sourceSide;
};

// Which axis the seam run is tiling along. A single-axis pass is the only way
// this works: a copy shifted on both axes has a discontinuity line per axis,
// and the region that uses it would have to cross one of them.
enum class SeamAxis
{
    Horizontal,
    Vertical
};

// Decides, for every pixel, whether the seam run should take it from the source
// (255) or from the copy shifted by half a tile along `axis` (0).
//
// The two copies have to meet somewhere, and where they meet is visible: a
// fixed line cuts through whatever happens to be there. This picks the meeting
// line as the minimum cut through "how different are the two copies here", so
// the switch happens where they already agree - along grout lines, between
// bricks, in flat paint - and costs nothing where it cannot be helped.
//
// Along `axis` the strips at both edges are pinned to the shifted copy and a
// band around the middle to the source; the cut is free in between, and it
// follows the content. Pinning the middle is not optional - it is exactly where
// the shifted copy has its own discontinuity, so that band must come from the
// source for the output to be continuous at all.
//
// Returns a full-size Format_Grayscale8 image, or a null image if the source is
// too small to tile along this axis.
QImage buildSeamlessCutMask(const QImage& image, SeamAxis axis);
