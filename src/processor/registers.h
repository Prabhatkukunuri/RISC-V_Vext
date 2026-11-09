/**
 * @file registers.h
 * @brief Contains the definition of the RegisterFile class for managing registers in the simulator.
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#ifndef REGISTERS_H
#define REGISTERS_H

#include <QObject>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Kites
{
/**
 * @brief Represents a register file containing integer, floating-point, and vector registers.
 */
class RegisterFile : public QObject
{
    Q_OBJECT
  private:
    static constexpr size_t NUM_GPR = 32; ///< Number of General-Purpose Registers (GPR).
    static constexpr size_t NUM_FPR = 32; ///< Number of Floating-Point Registers (FPR).

    std::array<uint64_t, NUM_GPR> gpr_ = {}; ///< Array for storing GPR values.
    std::array<uint64_t, NUM_FPR> fpr_ = {}; ///< Array for storing FPR values.

    static constexpr size_t NUM_CSR = 4096; ///< Number of Control and Status Registers (CSR).

    std::array<uint64_t, NUM_CSR> csr_ = {}; ///< Array for storing CSR values.

    /**
     * @brief Vector register file (v0-v31) as one flat byte array, VLEN/8 bytes per register.
     *
     * Element i of width SEW in vN lives at byte N * VLEN/8 + i * SEW/8, little-endian, so a
     * register group (LMUL > 1) is just a longer run of the same array.
     */
    std::vector<uint8_t> m_vectorRegisters;
    size_t m_vlenBytes = 0; ///< VLEN/8, latched from the VM config on Reset().

    void CheckVectorElement(size_t reg, size_t index, size_t sew) const;

  public:
    static constexpr size_t NUM_VR = 32; ///< Number of vector registers.

    /// Vector CSR addresses (RVV 1.0 spec, section 3).
    static constexpr size_t CSR_VSTART = 0x008;
    static constexpr size_t CSR_VXSAT = 0x009;
    static constexpr size_t CSR_VXRM = 0x00A;
    static constexpr size_t CSR_VCSR = 0x00F;
    static constexpr size_t CSR_VL = 0xC20;
    static constexpr size_t CSR_VTYPE = 0xC21;
    static constexpr size_t CSR_VLENB = 0xC22;

    /// vtype.vill: set when the requested vtype is unsupported (bit XLEN-1).
    static constexpr uint64_t VTYPE_VILL = 1ULL << 63;

    /**
     * @brief Enum representing the type of a register.
     */
    enum class RegisterType
    {
        INVALID,        ///< Invalid register type.
        INTEGER,        ///< General-purpose integer register.
        FLOATING_POINT, ///< Floating-point register.
        VECTOR,         ///< Vector register.
        CSR             ///< Control and Status Register (CSR).
    };

    explicit RegisterFile(QObject *parent = nullptr);
    virtual ~RegisterFile() = default;

    void Reset();

    /**
     * @brief Reads the value of a General-Purpose Register (GPR).
     * @param reg The index of the GPR to read.
     * @return The value of the GPR at the specified index.
     */
    [[nodiscard]] uint64_t ReadGpr(size_t reg) const;

    /**
     * @brief Writes a value to a General-Purpose Register (GPR).
     * @param reg The index of the GPR to write.
     * @param value The value to write.
     */
    void WriteGpr(size_t reg, uint64_t value);

    /**
     * @brief Reads the value of a Floating-Point Register (FPR).
     * @param reg The index of the FPR to read.
     * @return The value of the FPR at the specified index.
     */
    [[nodiscard]] uint64_t ReadFpr(size_t reg) const;

    /**
     * @brief Writes a value to a Floating-Point Register (FPR).
     * @param reg The index of the FPR to write.
     * @param value The value to write.
     */
    void WriteFpr(size_t reg, uint64_t value);

    [[nodiscard]] uint64_t ReadCsr(size_t reg) const;

    void WriteCsr(size_t reg, uint64_t value);

    /**
     * @brief Retrieves the values of all General-Purpose Registers (GPR).
     * @return A vector containing the values of all GPRs.
     */
    [[nodiscard]] std::vector<uint64_t> GetGprValues() const;

    /**
     * @brief Retrieves the values of all Floating-Point Registers (FPR).
     * @return A vector containing the values of all FPRs.
     */
    [[nodiscard]] std::vector<uint64_t> GetFprValues() const;

    /**
     * @brief Returns VLEN/8, the size of one vector register in bytes.
     */
    [[nodiscard]] size_t GetVlenBytes() const;

    /**
     * @brief Reads element @p index of width @p sew bits (8, 16, 32 or 64) from vector register
     * @p reg, zero-extended to 64 bits. An index past the end of @p reg continues into the next
     * registers, so LMUL > 1 groups work without a separate accessor.
     */
    [[nodiscard]] uint64_t ReadVectorElement(size_t reg, size_t index, size_t sew) const;

    /**
     * @brief Writes the low @p sew bits of @p value to element @p index of vector register @p reg.
     */
    void WriteVectorElement(size_t reg, size_t index, size_t sew, uint64_t value);

    /**
     * @brief Reads mask bit @p index of vector register @p reg (bit i is element i's mask).
     */
    [[nodiscard]] bool ReadMaskBit(size_t reg, size_t index) const;

    /**
     * @brief Writes mask bit @p index of vector register @p reg, leaving the other bits alone.
     */
    void WriteMaskBit(size_t reg, size_t index, bool value);

    /**
     * @brief Returns the VLEN/8 bytes of vector register @p reg, byte 0 first.
     */
    [[nodiscard]] std::vector<uint8_t> ReadVectorRegister(size_t reg) const;

    /**
     * @brief Overwrites vector register @p reg; @p bytes must be exactly VLEN/8 bytes long.
     */
    void WriteVectorRegister(size_t reg, const std::vector<uint8_t> &bytes);

    void ModifyRegister(const std::string &reg_name, uint64_t value);

  signals:
    void updateRegister(size_t regIndex, uint64_t value);
    void updateFRegister(size_t regIndex, uint64_t value);
    void updateVRegister(size_t regIndex);
    /// A CSR was written (e.g. vl/vtype by vset*, or vl trimmed by a fault-only-first load).
    void updateCsrRegister(size_t csrAddress);
    void registerResetSignal();
};

extern const std::unordered_set<std::string> valid_vector_registers;

/**
 * @brief True for CSRs that the CSR instructions may not write: address bits [11:10] == 0b11,
 * which includes vl, vtype and vlenb.
 */
bool IsReadOnlyCsr(size_t csr_address);

bool IsValidVectorRegister(const std::string &reg);

extern const std::unordered_set<std::string> valid_general_purpose_registers;

extern const std::unordered_set<std::string> valid_floating_point_registers;

extern const std::unordered_set<std::string> valid_csr_registers;

extern const std::unordered_map<std::string, int> csr_to_address;

/**
 * @brief Map of register aliases to their actual names.
 */
extern const std::unordered_map<std::string, std::string> reg_alias_to_name;

bool IsValidGeneralPurposeRegister(const std::string &reg);

bool IsValidFloatingPointRegister(const std::string &reg);

bool IsValidCsr(const std::string &reg);
}//namespace Kites
#endif // REGISTERS_H