#pragma once

namespace tsukuyomi::perf {

enum class Slot : int {
    Schematica = 0,
    Load,
    Cells,
    Draw,
    Clear,
    Prune,
    Diff,
    Review,
    Restore,
    FixTaken,
    PlaceActors,
    Dirty,
    Publish,
    MeshLookup,
    MeshTessellate,
    ChunkBuild,
    AskBuilds,

    HandRestock,
    HrResolve,
    HrCount,
    HrWatch,
    HrClientSide,
    Count
};

bool on();

long long now();

void add(Slot slot, long long began);

void endFrame();

void maybeReport();

void reset();

class Scope {
public:
    explicit Scope(Slot slot) : m_slot(slot), m_began(now()) {}
    ~Scope() { add(m_slot, m_began); }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

private:
    Slot m_slot;
    long long m_began;
};

}
