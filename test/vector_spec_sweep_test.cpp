// Spec sweep for the RVV integer instructions: every arithmetic, compare, carry, widening,
// narrowing, extension and reduction instruction is run at every SEW it is legal for (e8, e16,
// e32, e64) on random data salted with edge values, unmasked and masked, and each destination
// element is compared with a reference model written directly from the RVV 1.0 formulas.
//
// Every run uses vl = VLMAX - 1, so the last element is tail and must stay undisturbed; masked
// runs check that inactive elements keep their old value (Kites, like Spike, implements the
// agnostic policies as undisturbed). The reference model is independent of the simulator: it
// works on raw SEW-bit values with explicit sign handling and 128-bit products.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "common/instructions.h"
#include "processor/vector/vector_config.h"
#include "processor/registers.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;
using u64 = uint64_t;
using i64 = int64_t;
using u128 = unsigned __int128;
using i128 = __int128;

constexpr u64 kVlenBits = 128;
constexpr uint32_t kVd = 8, kVs2 = 16, kVs1 = 24; // register groups never overlap
constexpr int kScalar = 10;                       // x10 holds the .vx operand

u64 Mask(u64 w)
{
    return w >= 64 ? ~0ULL : (1ULL << w) - 1;
}
i64 S(u64 v, u64 w) // value as a signed w-bit number
{
    v &= Mask(w);
    if (w >= 64)
        return static_cast<i64>(v);
    const u64 sign = 1ULL << (w - 1);
    return static_cast<i64>((v ^ sign) - sign);
}
u64 U(u64 v, u64 w)
{
    return v & Mask(w);
}

// Random values salted with the edge cases the spec singles out.
class Data
{
  public:
    explicit Data(u64 seed) : m_rng(seed) {}
    u64 operator()(u64 w)
    {
        const u64 edges[] = {0, 1, Mask(w), Mask(w) >> 1, (Mask(w) >> 1) + 1, 2, Mask(w) - 1};
        const u64 pick = m_rng() % 10;
        return U(pick < 7 ? edges[pick] : m_rng(), w);
    }
    bool Bit()
    {
        return (m_rng() & 1) != 0;
    }

  private:
    std::mt19937_64 m_rng;
};

struct Outcome
{
    std::unique_ptr<Kites::RVSSProcessor> vm;
    std::vector<uint8_t> before[32];
};

// Runs "vsetvli (vl = VLMAX - 1), e<sew>, m<lmul>" then the instruction, after setup().
Outcome Execute(const std::string &instruction, u64 sew, int lmulLog2,
            const std::function<void(RegisterFile &)> &setup)
{
    Kites::setupVmStateDirectory();
    const u64 vlmax = (kVlenBits / sew) << lmulLog2;
    std::ostringstream src;
    src << "li x6, " << vlmax - 1 << "\n"
        << "vsetvli x5, x6, e" << sew << ", m" << (1 << lmulLog2) << ", ta, ma\n"
        << instruction << "\n";
    std::istringstream stream(src.str());
    Outcome run;
    run.vm = std::make_unique<Kites::RVSSProcessor>();
    run.vm->LoadProgram(Kites::assemble(stream));
    setup(run.vm->registers_);
    for (uint32_t r = 0; r < 32; ++r)
        run.before[r] = run.vm->registers_.ReadVectorRegister(r);
    run.vm->DebugRun();
    return run;
}

u64 OldElement(const Outcome &run, uint32_t reg, u64 index, u64 eew)
{
    // Element index of a group: walk the byte snapshot like RegisterFile does.
    const u64 bytes = kVlenBits / 8;
    const u64 offset = index * (eew / 8);
    const uint32_t r = reg + static_cast<uint32_t>(offset / bytes);
    u64 v = 0;
    for (u64 b = 0; b < eew / 8; ++b)
        v |= static_cast<u64>(run.before[r][offset % bytes + b]) << (8 * b);
    return v;
}
bool OldMaskBit(const Outcome &run, uint32_t reg, u64 i)
{
    return (run.before[reg][i / 8] >> (i % 8)) & 1;
}

// ------------------------------------------------------------------------------------------
// Single-width: vd[i] = f(vs2[i], src, vd_old[i]) at SEW
// ------------------------------------------------------------------------------------------
enum class Src
{
    VV,
    VX,
    VI,  // simm5
    VUI, // uimm5 (shifts)
};

struct SingleOp
{
    const char *name; // without suffix
    std::vector<Src> forms;
    std::function<u64(u64 a, u64 b, u64 old, u64 w)> f; // a = vs2, b = vs1 / rs1 / imm
    bool sourceFirst = false; // vmacc family: "vd, src, vs2"
};

u64 DivU(u64 a, u64 b, u64 w)
{
    return b == 0 ? Mask(w) : a / b;
}
u64 DivS(u64 a, u64 b, u64 w)
{
    const i64 x = S(a, w), y = S(b, w);
    if (y == 0)
        return Mask(w);
    if (y == -1)
        return U(0 - a, w); // MIN / -1 = MIN
    return U(static_cast<u64>(x / y), w);
}
u64 RemU(u64 a, u64 b, u64)
{
    return b == 0 ? a : a % b;
}
u64 RemS(u64 a, u64 b, u64 w)
{
    const i64 x = S(a, w), y = S(b, w);
    if (y == 0)
        return a;
    if (y == -1)
        return 0;
    return static_cast<u64>(x % y);
}
u64 HighSS(u64 a, u64 b, u64 w)
{
    return static_cast<u64>((static_cast<i128>(S(a, w)) * S(b, w)) >> w);
}
u64 HighUU(u64 a, u64 b, u64 w)
{
    return static_cast<u64>((static_cast<u128>(U(a, w)) * U(b, w)) >> w);
}
u64 HighSU(u64 a, u64 b, u64 w)
{
    return static_cast<u64>((static_cast<i128>(S(a, w)) * static_cast<i128>(U(b, w))) >> w);
}

std::vector<SingleOp> SingleOps()
{
    const auto vxi = std::vector<Src>{Src::VV, Src::VX, Src::VI};
    const auto vx = std::vector<Src>{Src::VV, Src::VX};
    const auto shifts = std::vector<Src>{Src::VV, Src::VX, Src::VUI};
    return {
        {"vadd", vxi, [](u64 a, u64 b, u64, u64) { return a + b; }},
        {"vsub", vx, [](u64 a, u64 b, u64, u64) { return a - b; }},
        {"vrsub", {Src::VX, Src::VI}, [](u64 a, u64 b, u64, u64) { return b - a; }},
        {"vand", vxi, [](u64 a, u64 b, u64, u64) { return a & b; }},
        {"vor", vxi, [](u64 a, u64 b, u64, u64) { return a | b; }},
        {"vxor", vxi, [](u64 a, u64 b, u64, u64) { return a ^ b; }},
        {"vsll", shifts, [](u64 a, u64 b, u64, u64 w) { return a << (b & (w - 1)); }},
        {"vsrl", shifts, [](u64 a, u64 b, u64, u64 w) { return U(a, w) >> (b & (w - 1)); }},
        {"vsra", shifts,
         [](u64 a, u64 b, u64, u64 w) { return static_cast<u64>(S(a, w) >> (b & (w - 1))); }},
        {"vminu", vx, [](u64 a, u64 b, u64, u64 w) { return U(a, w) < U(b, w) ? a : b; }},
        {"vmin", vx, [](u64 a, u64 b, u64, u64 w) { return S(a, w) < S(b, w) ? a : b; }},
        {"vmaxu", vx, [](u64 a, u64 b, u64, u64 w) { return U(a, w) > U(b, w) ? a : b; }},
        {"vmax", vx, [](u64 a, u64 b, u64, u64 w) { return S(a, w) > S(b, w) ? a : b; }},
        {"vmul", vx, [](u64 a, u64 b, u64, u64) { return a * b; }},
        {"vmulh", vx, [](u64 a, u64 b, u64, u64 w) { return HighSS(a, b, w); }},
        {"vmulhu", vx, [](u64 a, u64 b, u64, u64 w) { return HighUU(a, b, w); }},
        {"vmulhsu", vx, [](u64 a, u64 b, u64, u64 w) { return HighSU(a, b, w); }},
        {"vdivu", vx, [](u64 a, u64 b, u64 , u64 w) { return DivU(U(a, w), U(b, w), w); }},
        {"vdiv", vx, [](u64 a, u64 b, u64, u64 w) { return DivS(a, b, w); }},
        {"vremu", vx, [](u64 a, u64 b, u64, u64 w) { return RemU(U(a, w), U(b, w), w); }},
        {"vrem", vx, [](u64 a, u64 b, u64, u64 w) { return RemS(a, b, w); }},
        {"vmacc", vx, [](u64 a, u64 b, u64 d, u64) { return d + b * a; }, true},
        {"vnmsac", vx, [](u64 a, u64 b, u64 d, u64) { return d - b * a; }, true},
        {"vmadd", vx, [](u64 a, u64 b, u64 d, u64) { return b * d + a; }, true},
        {"vnmsub", vx, [](u64 a, u64 b, u64 d, u64) { return a - b * d; }, true},
    };
}

std::string Suffix(Src s)
{
    switch (s)
    {
    case Src::VV:
        return ".vv";
    case Src::VX:
        return ".vx";
    default:
        return ".vi";
    }
}

// The source operand: register text and its SEW-bit value (simm5 sign-extended, x truncated).
struct Source
{
    std::string text;
    u64 scalar = 0; // for .vx / .vi
};
Source MakeSource(Src form, Data &data, u64 w, u64 &rs1)
{
    switch (form)
    {
    case Src::VV:
        return {"v" + std::to_string(kVs1)};
    case Src::VX:
        rs1 = data(64); // a full 64-bit x register: only its low SEW bits count
        return {"x" + std::to_string(kScalar), U(rs1, w)};
    case Src::VI:
    {
        const int imm = static_cast<int>(data(64) % 32) - 16;
        return {std::to_string(imm), U(static_cast<u64>(static_cast<i64>(imm)), w)};
    }
    case Src::VUI:
    {
        const int imm = static_cast<int>(data(64) % 32);
        return {std::to_string(imm), static_cast<u64>(imm)};
    }
    }
    return {};
}

void FillRandom(RegisterFile &rf, uint32_t reg, u64 count, u64 w, Data &data)
{
    for (u64 i = 0; i < count; ++i)
        rf.WriteVectorElement(reg, i, w, data(w));
}
void FillMask(RegisterFile &rf, Data &data)
{
    for (u64 i = 0; i < kVlenBits; ++i)
        rf.WriteMaskBit(0, i, data.Bit());
}

constexpr u64 kSews[] = {8, 16, 32, 64};
} // namespace

TEST(VectorSpecSweepTest, SingleWidthArithmeticAtEverySew)
{
    Data data(1);
    for (const SingleOp &op : SingleOps())
        for (Src form : op.forms)
            for (u64 w : kSews)
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    const std::string vd = "v" + std::to_string(kVd);
                    const std::string vs2 = "v" + std::to_string(kVs2);
                    const std::string text =
                        std::string(op.name) + Suffix(form) + " " + vd + ", " +
                        (op.sourceFirst ? src.text + ", " + vs2 : vs2 + ", " + src.text) +
                        (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, vlmax, w, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    const RegisterFile &rf = run.vm->registers_;
                    for (u64 i = 0; i < vlmax; ++i)
                    {
                        const u64 old = OldElement(run, kVd, i, w);
                        const bool active = i < vl && (!masked || OldMaskBit(run, 0, i));
                        u64 expected = old;
                        if (active)
                        {
                            const u64 a = OldElement(run, kVs2, i, w);
                            const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w)
                                                          : src.scalar;
                            expected = U(op.f(a, b, old, w), w);
                        }
                        ASSERT_EQ(rf.ReadVectorElement(kVd, i, w), expected) << "element " << i;
                    }
                }
}

TEST(VectorSpecSweepTest, MergeMoveAndCarryAtEverySew)
{
    Data data(2);
    struct Op
    {
        const char *name;
        std::vector<Src> forms;
        bool usesV0;  // vmerge / vadc / vsbc: ", v0" operand, every body element written
        int kind;     // 0 merge, 1 move, 2 adc, 3 sbc
    };
    const std::vector<Op> ops = {
        {"vmerge", {Src::VV, Src::VX, Src::VI}, true, 0},
        {"vmv.v", {Src::VV, Src::VX, Src::VI}, false, 1},
        {"vadc", {Src::VV, Src::VX, Src::VI}, true, 2},
        {"vsbc", {Src::VV, Src::VX}, true, 3},
    };
    for (const Op &op : ops)
        for (Src form : op.forms)
            for (u64 w : kSews)
            {
                const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                u64 rs1 = 0;
                const Source src = MakeSource(form, data, w, rs1);
                std::string text;
                if (op.kind == 1)
                    text = std::string("vmv.v.") + Suffix(form).substr(2) + " v8, " + src.text;
                else
                    text = std::string(op.name) + Suffix(form) + "m v8, v16, " + src.text + ", v0";
                SCOPED_TRACE(text + " at e" + std::to_string(w));
                Outcome run = Execute(text, w, 0,
                                  [&](RegisterFile &rf)
                                  {
                                      FillRandom(rf, kVd, vlmax, w, data);
                                      FillRandom(rf, kVs2, vlmax, w, data);
                                      FillRandom(rf, kVs1, vlmax, w, data);
                                      FillMask(rf, data);
                                      rf.WriteGpr(kScalar, rs1);
                                  });
                for (u64 i = 0; i < vlmax; ++i)
                {
                    u64 expected = OldElement(run, kVd, i, w);
                    if (i < vl)
                    {
                        const u64 a = OldElement(run, kVs2, i, w);
                        const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                        const u64 m = OldMaskBit(run, 0, i);
                        switch (op.kind)
                        {
                        case 0: expected = m ? b : a; break;
                        case 1: expected = b; break;
                        case 2: expected = U(a + b + m, w); break;
                        default: expected = U(a - b - m, w); break;
                        }
                    }
                    ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, w), expected)
                        << "element " << i;
                }
            }
}

TEST(VectorSpecSweepTest, CompareAndCarryOutMasksAtEverySew)
{
    Data data(3);
    struct Op
    {
        const char *name;
        std::vector<Src> forms;
        std::function<bool(u64 a, u64 b, u64 w)> f;
    };
    const std::vector<Op> compares = {
        {"vmseq", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) == U(b, w); }},
        {"vmsne", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) != U(b, w); }},
        {"vmsltu", {Src::VV, Src::VX}, [](u64 a, u64 b, u64 w) { return U(a, w) < U(b, w); }},
        {"vmslt", {Src::VV, Src::VX}, [](u64 a, u64 b, u64 w) { return S(a, w) < S(b, w); }},
        {"vmsleu", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) <= U(b, w); }},
        {"vmsle", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return S(a, w) <= S(b, w); }},
        {"vmsgtu", {Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) > U(b, w); }},
        {"vmsgt", {Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return S(a, w) > S(b, w); }},
    };
    for (const Op &op : compares)
        for (Src form : op.forms)
            for (u64 w : kSews)
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    const std::string text = std::string(op.name) + Suffix(form) + " v8, v16, " +
                                             src.text + (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, 16, 8, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    for (u64 i = 0; i < kVlenBits; ++i)
                    {
                        bool expected = OldMaskBit(run, kVd, i);
                        if (i < vl && (!masked || OldMaskBit(run, 0, i)))
                        {
                            const u64 a = OldElement(run, kVs2, i, w);
                            const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                            expected = op.f(a, b, w);
                        }
                        ASSERT_EQ(run.vm->registers_.ReadMaskBit(kVd, i), expected) << "bit " << i;
                    }
                }

    // vmadc / vmsbc: the exact sum or difference before truncation (carry / borrow out).
    for (bool borrow : {false, true})
        for (bool withCarryIn : {false, true})
            for (Src form : {Src::VV, Src::VX, Src::VI})
                for (u64 w : kSews)
                {
                    if (borrow && form == Src::VI)
                        continue; // no vmsbc.vi / vmsbc.vim
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    const std::string text = std::string(borrow ? "vmsbc" : "vmadc") + Suffix(form) +
                                             (withCarryIn ? "m" : "") + " v8, v16, " + src.text +
                                             (withCarryIn ? ", v0" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, 16, 8, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    for (u64 i = 0; i < kVlenBits; ++i)
                    {
                        bool expected = OldMaskBit(run, kVd, i);
                        if (i < vl)
                        {
                            const u128 a = U(OldElement(run, kVs2, i, w), w);
                            const u128 b =
                                U(form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar, w);
                            const u128 c = withCarryIn && OldMaskBit(run, 0, i) ? 1 : 0;
                            expected = borrow ? a < b + c : ((a + b + c) >> w) != 0;
                        }
                        ASSERT_EQ(run.vm->registers_.ReadMaskBit(kVd, i), expected) << "bit " << i;
                    }
                }
}

TEST(VectorSpecSweepTest, WideningArithmeticAtEveryLegalSew)
{
    Data data(4);
    struct Op
    {
        const char *name;
        bool wideVs2;     // .wv / .wx
        bool sourceFirst; // vwmacc*: vd, src, vs2
        bool vs2Signed, srcSigned;
        std::function<u64(u64 a, u64 b, u64 old)> f; // a, b already extended to 64 bits
        std::vector<Src> forms = {Src::VV, Src::VX};
    };
    const auto add = [](u64 a, u64 b, u64) { return a + b; };
    const auto sub = [](u64 a, u64 b, u64) { return a - b; };
    const auto mul = [](u64 a, u64 b, u64) { return a * b; };
    const auto macc = [](u64 a, u64 b, u64 d) { return d + a * b; };
    const std::vector<Op> ops = {
        {"vwaddu", false, false, false, false, add}, {"vwadd", false, false, true, true, add},
        {"vwsubu", false, false, false, false, sub}, {"vwsub", false, false, true, true, sub},
        {"vwaddu", true, false, false, false, add},  {"vwadd", true, false, true, true, add},
        {"vwsubu", true, false, false, false, sub},  {"vwsub", true, false, true, true, sub},
        {"vwmulu", false, false, false, false, mul}, {"vwmul", false, false, true, true, mul},
        {"vwmulsu", false, false, true, false, mul},
        {"vwmaccu", false, true, false, false, macc}, {"vwmacc", false, true, true, true, macc},
        {"vwmaccsu", false, true, false, true, macc},
        {"vwmaccus", false, true, true, false, macc, {Src::VX}},
    };
    for (const Op &op : ops)
        for (Src form : op.forms)
            for (u64 w : {8, 16, 32})
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1, ww = 2 * w;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    std::string suffix = Suffix(form);
                    if (op.wideVs2)
                        suffix[1] = 'w';
                    const std::string text =
                        std::string(op.name) + suffix + " v8, " +
                        (op.sourceFirst ? src.text + ", v16" : "v16, " + src.text) +
                        (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, vlmax, ww, data);
                                          FillRandom(rf, kVs2, vlmax, op.wideVs2 ? ww : w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    const auto ext = [](u64 v, u64 bits, bool isSigned)
                    { return isSigned ? static_cast<u64>(S(v, bits)) : U(v, bits); };
                    for (u64 i = 0; i < vlmax; ++i)
                    {
                        const u64 old = OldElement(run, kVd, i, ww);
                        u64 expected = old;
                        if (i < vl && (!masked || OldMaskBit(run, 0, i)))
                        {
                            const u64 a = op.wideVs2 ? OldElement(run, kVs2, i, ww)
                                                     : ext(OldElement(run, kVs2, i, w), w,
                                                           op.vs2Signed);
                            const u64 rawB =
                                form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                            expected = U(op.f(a, ext(rawB, w, op.srcSigned), old), ww);
                        }
                        ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, ww), expected)
                            << "element " << i;
                    }
                }
}

TEST(VectorSpecSweepTest, NarrowingShiftsAndExtensionsAtEveryLegalSew)
{
    Data data(5);
    for (bool arithmetic : {false, true})
        for (Src form : {Src::VV, Src::VX, Src::VUI})
            for (u64 w : {8, 16, 32})
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1, ww = 2 * w;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    std::string suffix = Suffix(form);
                    suffix[1] = 'w';
                    const std::string text = std::string(arithmetic ? "vnsra" : "vnsrl") + suffix +
                                             " v8, v16, " + src.text + (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, vlmax, w, data);
                                          FillRandom(rf, kVs2, vlmax, ww, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    for (u64 i = 0; i < vlmax; ++i)
                    {
                        u64 expected = OldElement(run, kVd, i, w);
                        if (i < vl && (!masked || OldMaskBit(run, 0, i)))
                        {
                            const u64 wide = OldElement(run, kVs2, i, ww);
                            const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                            const unsigned sh = static_cast<unsigned>(b & (ww - 1)); // log2(2*SEW)
                            expected = U(arithmetic ? static_cast<u64>(S(wide, ww) >> sh)
                                                    : U(wide, ww) >> sh,
                                         w);
                        }
                        ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, w), expected)
                            << "element " << i;
                    }
                }

    for (unsigned factor : {2u, 4u, 8u})
        for (bool isSigned : {false, true})
            for (u64 w : kSews)
            {
                if (w / factor < 8)
                    continue; // source EEW below 8 is illegal (checked elsewhere)
                const u64 vlmax = kVlenBits / w, vl = vlmax - 1, nw = w / factor;
                const std::string text = std::string(isSigned ? "vsext" : "vzext") + ".vf" +
                                         std::to_string(factor) + " v8, v16, v0.t";
                SCOPED_TRACE(text + " at e" + std::to_string(w));
                Outcome run = Execute(text, w, 0,
                                  [&](RegisterFile &rf)
                                  {
                                      FillRandom(rf, kVd, vlmax, w, data);
                                      FillRandom(rf, kVs2, vlmax, nw, data);
                                      FillMask(rf, data);
                                  });
                for (u64 i = 0; i < vlmax; ++i)
                {
                    u64 expected = OldElement(run, kVd, i, w);
                    if (i < vl && OldMaskBit(run, 0, i))
                    {
                        const u64 v = OldElement(run, kVs2, i, nw);
                        expected = U(isSigned ? static_cast<u64>(S(v, nw)) : U(v, nw), w);
                    }
                    ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, w), expected)
                        << "element " << i;
                }
            }
}

TEST(VectorSpecSweepTest, ReductionsAtEverySewAndLmul)
{
    Data data(6);
    struct Op
    {
        const char *name;
        bool widening, isSigned;
        std::function<u64(u64 acc, u64 x, u64 w)> f; // values already extended
    };
    const std::vector<Op> ops = {
        {"vredsum", false, false, [](u64 a, u64 x, u64) { return a + x; }},
        {"vredand", false, false, [](u64 a, u64 x, u64) { return a & x; }},
        {"vredor", false, false, [](u64 a, u64 x, u64) { return a | x; }},
        {"vredxor", false, false, [](u64 a, u64 x, u64) { return a ^ x; }},
        {"vredminu", false, false, [](u64 a, u64 x, u64 w) { return U(x, w) < U(a, w) ? x : a; }},
        {"vredmin", false, true, [](u64 a, u64 x, u64 w) { return S(x, w) < S(a, w) ? x : a; }},
        {"vredmaxu", false, false, [](u64 a, u64 x, u64 w) { return U(x, w) > U(a, w) ? x : a; }},
        {"vredmax", false, true, [](u64 a, u64 x, u64 w) { return S(x, w) > S(a, w) ? x : a; }},
        {"vwredsumu", true, false, [](u64 a, u64 x, u64) { return a + x; }},
        {"vwredsum", true, true, [](u64 a, u64 x, u64) { return a + x; }},
    };
    for (const Op &op : ops)
        for (u64 w : kSews)
            for (int lmulLog2 : {0, 1})
                for (bool masked : {false, true})
                {
                    if (op.widening && w == 64)
                        continue;
                    const u64 vlmax = (kVlenBits / w) << lmulLog2, vl = vlmax - 1;
                    const u64 accW = op.widening ? 2 * w : w;
                    const std::string text = std::string(op.name) + ".vs v8, v16, v24" +
                                             (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w) + " m" +
                                 std::to_string(1 << lmulLog2));
                    Outcome run = Execute(text, w, lmulLog2,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, kVlenBits / accW, accW, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, kVlenBits / accW, accW, data);
                                          FillMask(rf, data);
                                      });
                    const auto ext = [&](u64 v, u64 bits)
                    { return op.isSigned ? static_cast<u64>(S(v, bits)) : U(v, bits); };
                    u64 acc = ext(OldElement(run, kVs1, 0, accW), accW);
                    for (u64 i = 0; i < vl; ++i)
                        if (!masked || OldMaskBit(run, 0, i))
                            acc = op.f(acc, ext(OldElement(run, kVs2, i, w), w), accW);
                    ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, 0, accW), U(acc, accW));
                    for (u64 i = 1; i < kVlenBits / accW; ++i) // the rest of vd is tail
                        ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, accW),
                                  OldElement(run, kVd, i, accW))
                            << "element " << i;
                }
}

// ------------------------------------------------------------------------------------------
// Spec rules not tied to one instruction family
// ------------------------------------------------------------------------------------------

TEST(VectorSpecRulesTest, VlZeroAndVstartAtLeastVlWriteNothingAndClearVstart)
{
    // vl = 0: no element is written. vstart >= vl: likewise. Either way vstart ends at 0.
    for (const char *setVl : {"li x6, 0\nvsetvli x5, x6, e32, m1, ta, ma\n",
                              "vsetivli x0, 2, e32, m1, ta, ma\ncsrrwi x0, vstart, 3\n"})
        for (const char *instruction :
             {"vadd.vv v8, v16, v24", "vwadd.vv v8, v16, v24", "vmseq.vv v8, v16, v24",
              "vmand.mm v8, v16, v24", "vid.v v8", "vmerge.vvm v8, v16, v24, v0",
              "vnsrl.wi v8, v16, 1", "vmacc.vv v8, v24, v16"})
        {
            SCOPED_TRACE(std::string(setVl) + instruction);
            Kites::setupVmStateDirectory();
            std::istringstream stream(std::string(setVl) + instruction + "\n");
            Kites::RVSSProcessor vm;
            vm.LoadProgram(Kites::assemble(stream));
            for (uint32_t r : {8u, 9u})
                for (u64 i = 0; i < 4; ++i)
                    vm.registers_.WriteVectorElement(r, i, 32, 0x55555555);
            vm.DebugRun();
            for (uint32_t r : {8u, 9u})
                for (u64 i = 0; i < 4; ++i)
                    ASSERT_EQ(vm.registers_.ReadVectorElement(r, i, 32), 0x55555555u);
            EXPECT_EQ(vm.registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
        }
}

TEST(VectorSpecRulesTest, VlZeroForCountAndFind)
{
    // vcpop.m counts nothing and vfirst.m finds nothing when vl = 0.
    Kites::setupVmStateDirectory();
    std::istringstream stream("li x6, 0\nvsetvli x5, x6, e8, m1, ta, ma\n"
                              "vcpop.m x10, v16\nvfirst.m x11, v16\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.registers_.WriteVectorElement(16, 0, 8, 0xFF);
    vm.registers_.WriteGpr(10, 99);
    vm.DebugRun();
    EXPECT_EQ(vm.registers_.ReadGpr(10), 0u);
    EXPECT_EQ(vm.registers_.ReadGpr(11), ~0ULL);
}

TEST(VectorSpecRulesTest, MaskLogicalAndVidStartAtVstart)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetivli x0, 8, e16, m1, ta, ma\n"
                              "csrrwi x0, vstart, 3\n"
                              "vmxnor.mm v8, v16, v16\n" // all ones from bit 3
                              "csrrwi x0, vstart, 5\n"
                              "vid.v v9\n");             // elements 5..7 only
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    for (u64 i = 0; i < 8; ++i)
        vm.registers_.WriteVectorElement(9, i, 16, 0xAAAA);
    vm.DebugRun();
    EXPECT_EQ(vm.registers_.ReadVectorElement(8, 0, 8), 0xF8u); // bits 3..7
    for (u64 i = 0; i < 8; ++i)
        EXPECT_EQ(vm.registers_.ReadVectorElement(9, i, 16), i < 5 ? 0xAAAAu : i) << i;
    EXPECT_EQ(vm.registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VectorSpecRulesTest, FractionalLmulUsesFewerElements)
{
    // e16, mf2: VLMAX = 128 / 16 / 2 = 4, all held in one register.
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetvli x5, x0, e16, mf2, ta, ma\nvadd.vi v8, v16, 1\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    for (u64 i = 0; i < 8; ++i)
    {
        vm.registers_.WriteVectorElement(16, i, 16, i);
        vm.registers_.WriteVectorElement(8, i, 16, 0x7777);
    }
    vm.DebugRun();
    EXPECT_EQ(vm.registers_.ReadGpr(5), 4u);
    for (u64 i = 0; i < 8; ++i)
        EXPECT_EQ(vm.registers_.ReadVectorElement(8, i, 16), i < 4 ? i + 1 : 0x7777u) << i;
}

TEST(VectorSpecRulesTest, IotaAndIdAcrossAnLmulGroup)
{
    // e16, m2: 16 elements in v8-v9; viota counts the set bits of v16 below each element.
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetvli x5, x0, e16, m2, ta, ma\n"
                              "viota.m v8, v16\n"
                              "vid.v v12\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.registers_.WriteVectorElement(16, 0, 16, 0xA5C3); // bits of elements 0..15
    vm.DebugRun();
    u64 count = 0;
    for (u64 i = 0; i < 16; ++i)
    {
        EXPECT_EQ(vm.registers_.ReadVectorElement(8, i, 16), count) << i;
        count += (0xA5C3 >> i) & 1;
        EXPECT_EQ(vm.registers_.ReadVectorElement(12, i, 16), i) << i;
    }
}

TEST(VectorSpecRulesTest, MaskedSetFirstVariants)
{
    // v16 = 0b00110100, v0 = 0b11011011; active elements 0,1,3,4,6,7.
    // Active vs2 bits: el0 0, el1 0, el3 0, el4 1, el6 0, el7 0 -> first active one is el4.
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetivli x0, 8, e8, m1, ta, ma\n"
                              "vmsbf.m v8, v16, v0.t\n"
                              "vmsif.m v9, v16, v0.t\n"
                              "vmsof.m v10, v16, v0.t\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.registers_.WriteVectorElement(16, 0, 8, 0x34);
    vm.registers_.WriteVectorElement(0, 0, 8, 0xDB);
    for (uint32_t r : {8u, 9u, 10u})
        vm.registers_.WriteVectorElement(r, 0, 8, 0x24); // inactive bits 2, 5 are 1
    vm.DebugRun();
    // Active results: sbf el0,1,3 = 1, el4,6,7 = 0; sif adds el4; sof only el4.
    EXPECT_EQ(vm.registers_.ReadVectorElement(8, 0, 8), 0x0Bu | 0x24u);
    EXPECT_EQ(vm.registers_.ReadVectorElement(9, 0, 8), 0x1Bu | 0x24u);
    EXPECT_EQ(vm.registers_.ReadVectorElement(10, 0, 8), 0x10u | 0x24u);
}

// parseVectorArith rejects a mnemonic with no rvv::kVectorArith row ("Unsupported vector
// instruction"). This catches the mistake earlier: every mnemonic the syntax map routes there must
// either have a row or be one of the aliases/pseudos the parser rewrites to one.
TEST(VectorSpecRulesTest, EveryArithMnemonicHasADecodeRow)
{
    using Kites::instruction_set::SyntaxType;
    const std::set<SyntaxType> arithSyntaxes = {
        SyntaxType::O_VR_C_VR_C_VR,      SyntaxType::O_VR_C_VR_C_GPR,
        SyntaxType::O_VR_C_VR_C_I,       SyntaxType::O_VR_C_VR,
        SyntaxType::O_VR_C_GPR_C_VR,     SyntaxType::O_VR_C_VR_C_VR_C_V0,
        SyntaxType::O_VR_C_VR_C_GPR_C_V0, SyntaxType::O_VR_C_VR_C_I_C_V0,
        SyntaxType::O_VR_C_GPR,          SyntaxType::O_VR_C_I,
        SyntaxType::O_GPR_C_VR,          SyntaxType::O_VR};
    const std::set<std::string> rewritten = {
        "vmsgt.vv", "vmsgtu.vv", "vmsge.vv", "vmsgeu.vv", "vmsge.vi", "vmsgeu.vi",
        "vmslt.vi", "vmsltu.vi", "vmmv.m",   "vmnot.m",   "vmclr.m",  "vmset.m"};
    size_t checked = 0;
    for (const auto &[mnemonic, syntaxes] : Kites::instruction_set::instruction_syntax_map)
    {
        bool routed = false;
        for (SyntaxType syntax : syntaxes)
        {
            routed = routed || arithSyntaxes.count(syntax) != 0;
        }
        if (!routed)
        {
            continue;
        }
        ++checked;
        EXPECT_TRUE(Kites::rvv::FindVectorArith(mnemonic) != nullptr || rewritten.count(mnemonic))
            << mnemonic << " reaches parseVectorArith but has no kVectorArith row";
    }
    EXPECT_GT(checked, 150u);
}
