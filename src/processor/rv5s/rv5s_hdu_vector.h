/**
 * @file rv5s_hdu_vector.h
 * @brief Hazard detection for vector instructions (and the CSR hazards they create) in the
 * hazard-detecting, forwarding 5-stage pipeline (H_F).
 *
 * Runs after EX, like check_data_hazard(): EX/MEM holds the instruction that has just executed
 * (the producer P1), IF/ID the instruction about to be decoded (the consumer C). Everything else
 * is forwarded, or has already been written back by the time C needs it, so one stall cycle is
 * always enough. The stalls it raises:
 *   - vector load -> vector instruction reading the loaded group in EX        (vector load-use)
 *   - vle*ff      -> vector instruction reading vl in EX, or csrr* of vl      (vl known after MEM)
 *   - scalar load -> vector instruction using that GPR as x[rs1]/x[rs2]       (GPR load-use)
 *   - Zicsr write of fflags/frm/fcsr -> F/D instruction (reads frm and writes fflags in EX,
 *     while the CSR write lands in WB)
 */
#pragma once

#include "processor/pipeline_registers.h"

namespace Kites
{
class RegisterFile;

/**
 * @brief Number of stall cycles (0 or 1) the vector/CSR hazards between EX/MEM and IF/ID need.
 *
 * @param mem_wb Only for its vtype: a vset* in MEM/WB writes back before C reaches EX, so C's
 * register groups must be sized with it.
 * @param registers The register file, for the current vtype.
 */
int check_vector_hazard(const IF_ID_Register &if_id, const EX_MEM_Register &ex_mem,
                        const MEM_WB_Register &mem_wb, const RegisterFile &registers);

/**
 * @brief The vtype the instruction now in IF/ID will execute with: a vset* in EX/MEM, else one
 * in MEM/WB, else the register file's.
 */
uint64_t vtype_seen_by_next_instruction(const EX_MEM_Register &ex_mem,
                                        const MEM_WB_Register &mem_wb,
                                        const RegisterFile &registers);
} // namespace Kites
