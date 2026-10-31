#include "game/StructureWrite.h"

#include <limits>
#include <stdexcept>

namespace tsukuyomi::structure {
namespace {
class Writer {
    std::ostream& out;
public:
    explicit Writer(std::ostream& stream) : out(stream) {}
    void byte(unsigned n) { out.put(static_cast<char>(n)); }
    void integer(std::int32_t n) { const auto u=static_cast<std::uint32_t>(n); for (unsigned i=0;i<4;++i) byte((u>>(8*i))&255); }
    void text(const std::string& s) {
        if (s.size()>UINT16_MAX) throw std::runtime_error("NBT string is too long");
        byte(static_cast<unsigned>(s.size())&255); byte(static_cast<unsigned>(s.size())>>8);
        out.write(s.data(),static_cast<std::streamsize>(s.size()));
    }
    void named(nbt::Tag tag,const std::string& name) { byte(static_cast<unsigned>(tag)); text(name); }
    void compound(const char* name) { named(nbt::Tag::Compound,name); }
    void end() { byte(0); }
    void list(const char* name,nbt::Tag tag,std::int32_t count) { named(nbt::Tag::List,name); byte(static_cast<unsigned>(tag)); integer(count); }
    void intTag(const char* name,std::int32_t n) { named(nbt::Tag::Int,name); integer(n); }
};
}
bool write(std::ostream& out,const Structure& s,const std::int32_t origin[3],std::string& why)
{
    why.clear();
    if (!s.valid() || s.volume()!=s.blocks.size() || s.volume()>INT32_MAX
        || (!s.blocks2.empty() && s.blocks2.size()!=s.blocks.size()) || s.palette.size()>65534) {
        why="Invalid structure dimensions or palette"; return false;
    }
    for (const auto* layer : {&s.blocks,&s.blocks2}) for (const auto v : *layer)
        if (v>s.palette.size()) { why="Invalid palette index"; return false; }
    try {
        Writer w(out);
        w.compound(""); w.intTag("format_version",1);
        w.list("size",nbt::Tag::Int,3); w.integer(s.sizeX); w.integer(s.sizeY); w.integer(s.sizeZ);
        w.compound("structure"); w.list("block_indices",nbt::Tag::List,2);
        for (int layer=0;layer<2;++layer) {
            w.byte(static_cast<unsigned>(nbt::Tag::Int)); w.integer(static_cast<std::int32_t>(s.volume()));
            for (std::size_t i=0;i<s.volume();++i) w.integer(layer==0?s.entryAt(i):s.entry2At(i));
        }
        w.list("entities",nbt::Tag::Compound,0);
        w.compound("palette"); w.compound("default");
        w.list("block_palette",nbt::Tag::Compound,static_cast<std::int32_t>(s.palette.size()));
        for (const auto& p : s.palette) {
            w.named(nbt::Tag::String,"name"); w.text(p.name); w.compound("states");
            for (const auto& state : p.states) {
                if (state.tag!=nbt::Tag::Byte && state.tag!=nbt::Tag::Int && state.tag!=nbt::Tag::String)
                    throw std::runtime_error("Unsupported block state type");
                w.named(state.tag,state.name);
                if (state.tag==nbt::Tag::String) w.text(state.text);
                else if (state.tag==nbt::Tag::Byte) {
                    if (state.number<INT8_MIN || state.number>INT8_MAX) throw std::runtime_error("Byte state is out of range");
                    w.byte(static_cast<unsigned>(state.number)&255);
                } else {
                    if (state.number<INT32_MIN || state.number>INT32_MAX) throw std::runtime_error("Int state is out of range");
                    w.integer(static_cast<std::int32_t>(state.number));
                }
            }
            w.end(); w.intTag("version",p.version); w.end();
        }
        w.compound("block_position_data"); w.end(); w.end(); w.end(); w.end();
        w.list("structure_world_origin",nbt::Tag::Int,3); for (int a=0;a<3;++a) w.integer(origin[a]); w.end();
        if (!out.good()) { why="Could not write structure"; return false; }
        return true;
    } catch (const std::exception& e) { why=e.what(); return false; }
}
}
