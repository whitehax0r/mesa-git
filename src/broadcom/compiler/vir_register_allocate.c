/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "util/ralloc.h"
#include "util/register_allocate.h"
#include "common/v3d_device_info.h"
#include "v3d_compiler.h"

#define ACC_INDEX     0
#define ACC_COUNT     6
#define PHYS_INDEX    (ACC_INDEX + ACC_COUNT)
#define PHYS_COUNT    64

#define CLASS_BITS_PHYS   (1 << 0)
#define CLASS_BITS_ACC    (1 << 1)
#define CLASS_BITS_R5     (1 << 4)
#define CLASS_BITS_ANY    (CLASS_BITS_PHYS | \
                           CLASS_BITS_ACC | \
                           CLASS_BITS_R5)

static inline uint32_t
temp_to_node(uint32_t temp)
{
        return temp + ACC_COUNT;
}

static inline uint32_t
node_to_temp(uint32_t node)
{
        assert(node >= ACC_COUNT);
        return node - ACC_COUNT;
}

static inline uint8_t
get_temp_class_bits(struct v3d_ra_node_info *nodes,
                    uint32_t temp)
{
        return nodes->info[temp_to_node(temp)].class_bits;
}

static inline void
set_temp_class_bits(struct v3d_ra_node_info *nodes,
                    uint32_t temp, uint8_t class_bits)
{
        nodes->info[temp_to_node(temp)].class_bits = class_bits;
}

static struct ra_class *
choose_reg_class(struct v3d_compile *c, uint8_t class_bits)
{
        if (class_bits == CLASS_BITS_PHYS) {
                return c->compiler->reg_class_phys[c->thread_index];
        } else if (class_bits == (CLASS_BITS_R5)) {
                return c->compiler->reg_class_r5[c->thread_index];
        } else if (class_bits == (CLASS_BITS_PHYS | CLASS_BITS_ACC)) {
                return c->compiler->reg_class_phys_or_acc[c->thread_index];
        } else {
                assert(class_bits == CLASS_BITS_ANY);
                return c->compiler->reg_class_any[c->thread_index];
        }
}

static inline struct ra_class *
choose_reg_class_for_temp(struct v3d_compile *c, uint32_t temp)
{
        assert(temp < c->num_temps && temp < c->nodes.alloc_count);
        return choose_reg_class(c, get_temp_class_bits(&c->nodes, temp));
}

static inline bool
qinst_writes_tmu(const struct v3d_device_info *devinfo,
                 struct qinst *inst)
{
        return (inst->dst.file == QFILE_MAGIC &&
                v3d_qpu_magic_waddr_is_tmu(devinfo, inst->dst.index)) ||
                inst->qpu.sig.wrtmuc;
}

static bool
is_end_of_tmu_sequence(const struct v3d_device_info *devinfo,
                       struct qinst *inst, struct qblock *block)
{
        /* Only tmuwt and ldtmu can finish TMU sequences */
        bool is_tmuwt = inst->qpu.type == V3D_QPU_INSTR_TYPE_ALU &&
                        inst->qpu.alu.add.op == V3D_QPU_A_TMUWT;
        bool is_ldtmu = inst->qpu.sig.ldtmu;
        if (!is_tmuwt && !is_ldtmu)
                return false;

        /* Check if this is the last tmuwt or ldtmu in the sequence */
        list_for_each_entry_from(struct qinst, scan_inst, inst->link.next,
                                 &block->instructions, link) {
                is_tmuwt = scan_inst->qpu.type == V3D_QPU_INSTR_TYPE_ALU &&
                           scan_inst->qpu.alu.add.op == V3D_QPU_A_TMUWT;
                is_ldtmu = scan_inst->qpu.sig.ldtmu;

                if (is_tmuwt || is_ldtmu)
                        return false;

                if (qinst_writes_tmu(devinfo, scan_inst))
                        return true;
        }

        return true;
}

static bool
vir_is_mov_uniform(struct v3d_compile *c, int temp)
{
        struct qinst *def = c->defs[temp];

        return def && def->qpu.sig.ldunif;
}

static bool
can_reconstruct_inst(struct qinst *inst)
{
        assert(inst);

        if (vir_is_add(inst)) {
                switch (inst->qpu.alu.add.op) {
                case V3D_QPU_A_FXCD:
                case V3D_QPU_A_FYCD:
                case V3D_QPU_A_XCD:
                case V3D_QPU_A_YCD:
                case V3D_QPU_A_IID:
                case V3D_QPU_A_EIDX:
                case V3D_QPU_A_TIDX:
                case V3D_QPU_A_SAMPID:
                        /* No need to check input unpacks because none of these
                         * opcodes read sources. FXCD,FYCD have pack variants.
                         */
                        return inst->qpu.flags.ac == V3D_QPU_COND_NONE &&
                               inst->qpu.flags.auf == V3D_QPU_UF_NONE &&
                               inst->qpu.flags.apf == V3D_QPU_PF_NONE &&
                               inst->qpu.alu.add.output_pack == V3D_QPU_PACK_NONE;
                default:
                        return false;
                }
        }

        return false;
}

static bool
can_reconstruct_temp(struct v3d_compile *c, int temp)
{
        struct qinst *def = c->defs[temp];
        return def && can_reconstruct_inst(def);
}

static struct qreg
reconstruct_temp(struct v3d_compile *c, enum v3d_qpu_add_op op)
{
        struct qreg dest;
        switch (op) {
        case V3D_QPU_A_FXCD:
                dest = vir_FXCD(c);
                break;
        case V3D_QPU_A_FYCD:
                dest = vir_FYCD(c);
                break;
        case V3D_QPU_A_XCD:
                dest = vir_XCD(c);
                break;
        case V3D_QPU_A_YCD:
                dest = vir_YCD(c);
                break;
        case V3D_QPU_A_IID:
                dest = vir_IID(c);
                break;
        case V3D_QPU_A_EIDX:
                dest = vir_EIDX(c);
                break;
        case V3D_QPU_A_TIDX:
                dest = vir_TIDX(c);
                break;
        case V3D_QPU_A_SAMPID:
                dest = vir_SAMPID(c);
                break;
        default:
            unreachable("Unexpected opcode for reconstruction");
        }

        return dest;
}

enum temp_spill_type {
        SPILL_TYPE_UNIFORM,
        SPILL_TYPE_RECONSTRUCT,
        SPILL_TYPE_TMU
};

static enum temp_spill_type
get_spill_type_for_temp(struct v3d_compile *c, int temp)
{
   if (vir_is_mov_uniform(c, temp))
      return SPILL_TYPE_UNIFORM;

   if (can_reconstruct_temp(c, temp))
      return SPILL_TYPE_RECONSTRUCT;

   return SPILL_TYPE_TMU;
}

static int
v3d_choose_spill_node(struct v3d_compile *c)
{
        const float tmu_scale = 10;
        float block_scale = 1.0;
        float spill_costs[c->num_temps];
        bool in_tmu_operation = false;
        bool started_last_seg = false;

        for (unsigned i = 0; i < c->num_temps; i++)
                spill_costs[i] = 0.0;

        /* XXX: Scale the cost up when inside of a loop. */
        vir_for_each_block(block, c) {
                vir_for_each_inst(inst, block) {
                        /* We can't insert new thread switches after
                         * starting output writes.
                         */
                        bool no_spilling =
                                (c->threads > 1 && started_last_seg) ||
                                (c->max_tmu_spills == 0);

                        /* Discourage spilling of TMU operations */
                        for (int i = 0; i < vir_get_nsrc(inst); i++) {
                                if (inst->src[i].file != QFILE_TEMP)
                                        continue;

                                int temp = inst->src[i].index;
                                enum temp_spill_type spill_type =
                                        get_spill_type_for_temp(c, temp);

                                if (spill_type != SPILL_TYPE_TMU) {
                                        spill_costs[temp] += block_scale;
                                } else if (!no_spilling) {
                                        float tmu_op_scale = in_tmu_operation ?
                                                3.0 : 1.0;
                                        spill_costs[temp] += (block_scale *
                                                              tmu_scale *
                                                              tmu_op_scale);
                                } else {
                                        BITSET_CLEAR(c->spillable, temp);
                                }
                        }

                        if (inst->dst.file == QFILE_TEMP) {
                                int temp = inst->dst.index;
                                enum temp_spill_type spill_type =
                                        get_spill_type_for_temp(c, temp);

                                if (spill_type != SPILL_TYPE_TMU) {
                                        /* We just rematerialize it later */
                                } else if (!no_spilling) {
                                        spill_costs[temp] += (block_scale *
                                                              tmu_scale);
                                } else {
                                        BITSET_CLEAR(c->spillable, temp);
                                }
                        }

                        /* Refuse to spill a ldvary's dst, because that means
                         * that ldvary's r5 would end up being used across a
                         * thrsw.
                         */
                        if (inst->qpu.sig.ldvary) {
                                assert(inst->dst.file == QFILE_TEMP);
                                BITSET_CLEAR(c->spillable, inst->dst.index);
                        }

                        if (inst->is_last_thrsw)
                                started_last_seg = true;

                        if (v3d_qpu_writes_vpm(&inst->qpu) ||
                            v3d_qpu_uses_tlb(&inst->qpu))
                                started_last_seg = true;

                        /* Track when we're in between a TMU setup and the
                         * final LDTMU or TMUWT from that TMU setup.  We
                         * penalize spills during that time.
                         */
                        if (is_end_of_tmu_sequence(c->devinfo, inst, block))
                                in_tmu_operation = false;

                        if (qinst_writes_tmu(c->devinfo, inst))
                                in_tmu_operation = true;
                }
        }

        for (unsigned i = 0; i < c->num_temps; i++) {
                if (BITSET_TEST(c->spillable, i)) {
                        ra_set_node_spill_cost(c->g, temp_to_node(i),
                                               spill_costs[i]);
                }
        }

        return ra_get_best_spill_node(c->g);
}

static void
ensure_nodes(struct v3d_compile *c)
{
        if (c->num_temps < c->nodes.alloc_count)
                return;

        c->nodes.alloc_count *= 2;
        c->nodes.info = reralloc_array_size(c,
                                            c->nodes.info,
                                            sizeof(c->nodes.info[0]),
                                            c->nodes.alloc_count + ACC_COUNT);
}

/* Creates the interference node for a new temp. We use this to keep the node
 * list updated during the spilling process, which generates new temps/nodes.
 */
static void
add_node(struct v3d_compile *c, uint32_t temp, uint8_t class_bits)
{
        ensure_nodes(c);

        int node = ra_add_node(c->g, choose_reg_class(c, class_bits));
        assert(node == temp + ACC_COUNT);

        /* We fill the node priority after we are done inserting spills */
        c->nodes.info[node].class_bits = class_bits;
        c->nodes.info[node].priority = 0;
}

/* The spill offset for this thread takes a bit of setup, so do it once at
 * program start.
 */
void
v3d_setup_spill_base(struct v3d_compile *c)
{
        /* Setting up the spill base is done in the entry block; so change
         * both the current block to emit and the cursor.
         */
        struct qblock *current_block = c->cur_block;
        c->cur_block = vir_entry_block(c);
        c->cursor = vir_before_block(c->cur_block);

        int start_num_temps = c->num_temps;

        /* Each thread wants to be in a separate region of the scratch space
         * so that the QPUs aren't fighting over cache lines.  We have the
         * driver keep a single global spill BO rather than
         * per-spilling-program BOs, so we need a uniform from the driver for
         * what the per-thread scale is.
         */
        struct qreg thread_offset =
                vir_UMUL(c,
                         vir_TIDX(c),
                         vir_uniform(c, QUNIFORM_SPILL_SIZE_PER_THREAD, 0));

        /* Each channel in a reg is 4 bytes, so scale them up by that. */
        struct qreg element_offset = vir_SHL(c, vir_EIDX(c),
                                             vir_uniform_ui(c, 2));

        c->spill_base = vir_ADD(c,
                                vir_ADD(c, thread_offset, element_offset),
                                vir_uniform(c, QUNIFORM_SPILL_OFFSET, 0));

        /* Make sure that we don't spill the spilling setup instructions. */
        for (int i = start_num_temps; i < c->num_temps; i++) {
                BITSET_CLEAR(c->spillable, i);

                /* If we are spilling, update the RA map with the temps added
                 * by the spill setup. Our spill_base register can never be an
                 * accumulator because it is used for TMU spill/fill and thus
                 * needs to persist across thread switches.
                 */
                if (c->spilling) {
                        int temp_class = CLASS_BITS_PHYS;
                        if (i != c->spill_base.index)
                                temp_class |= CLASS_BITS_ACC;
                        add_node(c, i, temp_class);
                }
        }

        /* Restore the current block. */
        c->cur_block = current_block;
        c->cursor = vir_after_block(c->cur_block);
}

/**
 * Computes the address for a spill/fill sequence and completes the spill/fill
 * sequence by emitting the following code:
 *
 * ldunif.spill_offset
 * add tmua spill_base spill_offset
 * thrsw
 *
 * If the sequence is for a spill, then it will emit a tmuwt after the thrsw,
 * otherwise it will emit an ldtmu to load the fill result into 'fill_dst'.
 *
 * The parameter 'ip' represents the ip at which the spill/fill is happening.
 * This is used to disallow accumulators on temps that cross this ip boundary
 * due to the new thrsw itroduced in the sequence above.
 */
static void
v3d_emit_spill_tmua(struct v3d_compile *c,
                    uint32_t spill_offset,
                    enum v3d_qpu_cond cond,
                    int32_t ip,
                    struct qreg *fill_dst)
{
        assert(ip >= 0);

        /* Load a uniform with the spill offset and add it to the spill base
         * to obtain the TMUA address. It can be of class ANY because we know
         * we are consuming it immediately without thrsw in between.
         */
        assert(c->disable_ldunif_opt);
        struct qreg offset = vir_uniform_ui(c, spill_offset);
        add_node(c, offset.index, CLASS_BITS_ANY);

        /* We always enable per-quad on spills/fills to ensure we spill
         * any channels involved with helper invocations.
         */
        struct qreg tmua = vir_reg(QFILE_MAGIC, V3D_QPU_WADDR_TMUAU);
        struct qinst *inst = vir_ADD_dest(c, tmua, c->spill_base, offset);
        inst->qpu.flags.ac = cond;
        inst->uniform = vir_get_uniform_index(c, QUNIFORM_CONSTANT,
                                              0xffffff7f); /* per-quad */

        vir_emit_thrsw(c);

        /* If this is for a spill, emit a TMUWT otherwise a LDTMU to load the
         * result of the fill. The TMUWT temp is not really read, the ldtmu
         * temp will be used immediately so just like the uniform above we
         * can allow accumulators.
         */
        if (!fill_dst) {
                struct qreg dst = vir_TMUWT(c);
                assert(dst.file == QFILE_TEMP);
                add_node(c, dst.index, CLASS_BITS_PHYS | CLASS_BITS_ACC);
        } else {
                *fill_dst = vir_LDTMU(c);
                assert(fill_dst->file == QFILE_TEMP);
                add_node(c, fill_dst->index, CLASS_BITS_PHYS | CLASS_BITS_ACC);
        }

        /* Temps across the thread switch we injected can't be assigned to
         * accumulators.
         *
         * Fills inject code before ip, so anything that starts at ip or later
         * is not affected by the thrsw. Something that ends at ip will be
         * affected though.
         *
         * Spills inject code after ip, so anything that starts strictly later
         * than ip is not affected (the temp starting at ip is usually the
         * spilled temp except for postponed spills). Something that ends at ip
         * won't be affected either.
         */
        for (int i = 0; i < c->spill_start_num_temps; i++) {
                bool thrsw_cross = fill_dst ?
                        c->temp_start[i] < ip && c->temp_end[i] >= ip :
                        c->temp_start[i] <= ip && c->temp_end[i] > ip;
                if (thrsw_cross) {
                        ra_set_node_class(c->g, temp_to_node(i),
                                          choose_reg_class(c, CLASS_BITS_PHYS));
                }
        }
}

static void
v3d_emit_tmu_spill(struct v3d_compile *c,
                   struct qinst *inst,
                   struct qreg spill_temp,
                   struct qinst *position,
                   uint32_t ip,
                   uint32_t spill_offset)
{
        assert(inst->qpu.type == V3D_QPU_INSTR_TYPE_ALU);
        assert(inst->dst.file == QFILE_TEMP);

        c->cursor = vir_after_inst(position);

        /* If inst and position don't match, this is a postponed spill,
         * in which case we have already allocated the temp for the spill
         * and we should use that, otherwise create a new temp with the
         * same register class bits as the original.
         */
        if (inst == position) {
                uint8_t class_bits = get_temp_class_bits(&c->nodes,
                                                         inst->dst.index);
                inst->dst = vir_get_temp(c);
                add_node(c, inst->dst.index, class_bits);
        } else {
                inst->dst = spill_temp;
        }

        enum v3d_qpu_cond cond = vir_get_cond(inst);
        struct qinst *tmp =
                vir_MOV_dest(c, vir_reg(QFILE_MAGIC, V3D_QPU_WADDR_TMUD),
                             inst->dst);
        tmp->qpu.flags.mc = cond;

        v3d_emit_spill_tmua(c, spill_offset, cond, ip, NULL);

        c->spills++;
        c->tmu_dirty_rcl = true;
}

static inline bool
interferes(int32_t t0_start, int32_t t0_end, int32_t t1_start, int32_t t1_end)
{
        return !(t0_start >= t1_end || t1_start >= t0_end);
}

static void
v3d_spill_reg(struct v3d_compile *c, int *acc_nodes, int spill_temp)
{
        c->spill_start_num_temps = c->num_temps;
        c->spilling = true;

        enum temp_spill_type spill_type = get_spill_type_for_temp(c, spill_temp);

        uint32_t spill_offset = 0;
        if (spill_type == SPILL_TYPE_TMU) {
                spill_offset = c->spill_size;
                c->spill_size += V3D_CHANNELS * sizeof(uint32_t);

                if (spill_offset == 0)
                        v3d_setup_spill_base(c);
        }

        struct qinst *last_thrsw = c->last_thrsw;
        assert(last_thrsw && last_thrsw->is_last_thrsw);

        int uniform_index = ~0;
        if (spill_type == SPILL_TYPE_UNIFORM) {
                struct qinst *orig_unif = c->defs[spill_temp];
                uniform_index = orig_unif->uniform;
        }

        enum v3d_qpu_add_op reconstruct_op = V3D_QPU_A_NOP;
        if (spill_type == SPILL_TYPE_RECONSTRUCT) {
                struct qinst *orig_def = c->defs[spill_temp];
                assert(vir_is_add(orig_def));
                reconstruct_op = orig_def->qpu.alu.add.op;
        }

        uint32_t spill_node = temp_to_node(spill_temp);

        /* We must disable the ldunif optimization if we are spilling uniforms */
        bool had_disable_ldunif_opt = c->disable_ldunif_opt;
        c->disable_ldunif_opt = true;

        struct qinst *start_of_tmu_sequence = NULL;
        struct qinst *postponed_spill = NULL;
        struct qreg postponed_spill_temp = { 0 };
        vir_for_each_block(block, c) {
                vir_for_each_inst_safe(inst, block) {
                        int32_t ip = inst->ip;

                        /* Track when we're in between a TMU setup and the final
                         * LDTMU or TMUWT from that TMU setup. We can't spill/fill any
                         * temps during that time, because that involves inserting a
                         * new TMU setup/LDTMU sequence, so we postpone the spill or
                         * move the fill up to not intrude in the middle of the TMU
                         * sequence.
                         */
                        if (is_end_of_tmu_sequence(c->devinfo, inst, block)) {
                                if (postponed_spill) {
                                        v3d_emit_tmu_spill(c, postponed_spill,
                                                           postponed_spill_temp,
                                                           inst, ip, spill_offset);
                                }

                                start_of_tmu_sequence = NULL;
                                postponed_spill = NULL;
                        }

                        if (!start_of_tmu_sequence &&
                            qinst_writes_tmu(c->devinfo, inst)) {
                                start_of_tmu_sequence = inst;
                        }

                        /* fills */
                        int filled_src = -1;
                        for (int i = 0; i < vir_get_nsrc(inst); i++) {
                                if (inst->src[i].file != QFILE_TEMP ||
                                    inst->src[i].index != spill_temp) {
                                        continue;
                                }

                                if (filled_src >= 0) {
                                        inst->src[i] = inst->src[filled_src];
                                        continue;
                                }

                                c->cursor = vir_before_inst(inst);

                                if (spill_type == SPILL_TYPE_UNIFORM) {
                                        struct qreg unif =
                                                vir_uniform(c,
                                                            c->uniform_contents[uniform_index],
                                                            c->uniform_data[uniform_index]);
                                        inst->src[i] = unif;
                                        /* We are using the uniform in the
                                         * instruction immediately after, so
                                         * we can use any register class for it.
                                         */
                                        add_node(c, unif.index, CLASS_BITS_ANY);
                                } else if (spill_type == SPILL_TYPE_RECONSTRUCT) {
                                        struct qreg temp =
                                                reconstruct_temp(c, reconstruct_op);
                                        inst->src[i] = temp;
                                        /* We are using the temp in the
                                         * instruction immediately after so we
                                         * can use ACC.
                                         */
                                        add_node(c, temp.index, CLASS_BITS_PHYS |
                                                                CLASS_BITS_ACC);
                                } else {
                                        /* If we have a postponed spill, we
                                         * don't need a fill as the temp would
                                         * not have been spilled yet, however,
                                         * we need to update the temp index.
                                         */
                                        if (postponed_spill) {
                                                inst->src[i] =
                                                        postponed_spill_temp;
                                        } else {
                                                int32_t fill_ip = ip;
                                                if (start_of_tmu_sequence) {
                                                        c->cursor = vir_before_inst(start_of_tmu_sequence);
                                                        fill_ip = start_of_tmu_sequence->ip;
                                                }

                                                v3d_emit_spill_tmua(c,  spill_offset,
                                                                    V3D_QPU_COND_NONE,
                                                                    fill_ip, &inst->src[i]);
                                                c->fills++;
                                        }
                                }

                                filled_src = i;
                        }

                        /* spills */
                        if (inst->dst.file == QFILE_TEMP &&
                            inst->dst.index == spill_temp) {
                                if (spill_type != SPILL_TYPE_TMU) {
                                        c->cursor.link = NULL;
                                        vir_remove_instruction(c, inst);
                                } else {
                                        /* If we are in the middle of a TMU
                                         * sequence, we postpone the actual
                                         * spill until we have finished it. We,
                                         * still need to replace the spill temp
                                         * with a new temp though.
                                         */
                                        if (start_of_tmu_sequence) {
                                                postponed_spill = inst;
                                                postponed_spill_temp =
                                                        vir_get_temp(c);
                                                add_node(c,
                                                         postponed_spill_temp.index,
                                                         c->nodes.info[spill_node].class_bits);
                                        } else {
                                                v3d_emit_tmu_spill(c, inst,
                                                                   postponed_spill_temp,
                                                                   inst, ip,
                                                                   spill_offset);
                                        }
                                }
                        }
                }
        }

        /* Make sure c->last_thrsw is the actual last thrsw, not just one we
         * inserted in our most recent unspill.
         */
        c->last_thrsw = last_thrsw;

        /* Don't allow spilling of our spilling instructions.  There's no way
         * they can help get things colored.
         */
        for (int i = c->spill_start_num_temps; i < c->num_temps; i++)
                BITSET_CLEAR(c->spillable, i);

        /* Reset interference for spilled node */
        ra_set_node_spill_cost(c->g, spill_node, 0);
        ra_reset_node_interference(c->g, spill_node);
        BITSET_CLEAR(c->spillable, spill_temp);

        /* Rebuild program ips */
        int32_t ip = 0;
        vir_for_each_inst_inorder(inst, c)
                inst->ip = ip++;

        /* Rebuild liveness */
        vir_calculate_live_intervals(c);

        /* Add interferences for the new spilled temps and update interferences
         * for c->spill_base (since we may have modified its liveness). Also,
         * update node priorities based one new liveness data.
         */
        uint32_t sb_temp =c->spill_base.index;
        uint32_t sb_node = temp_to_node(sb_temp);
        for (uint32_t i = 0; i < c->num_temps; i++) {
                if (c->temp_end[i] == -1)
                        continue;

                uint32_t node_i = temp_to_node(i);
                c->nodes.info[node_i].priority =
                        c->temp_end[i] - c->temp_start[i];

                for (uint32_t j = MAX2(i + 1, c->spill_start_num_temps);
                     j < c->num_temps; j++) {
                        if (interferes(c->temp_start[i], c->temp_end[i],
                                       c->temp_start[j], c->temp_end[j])) {
                                uint32_t node_j = temp_to_node(j);
                                ra_add_node_interference(c->g, node_i, node_j);
                        }
                }

                if (spill_type == SPILL_TYPE_TMU) {
                        if (i != sb_temp &&
                            interferes(c->temp_start[i], c->temp_end[i],
                                       c->temp_start[sb_temp], c->temp_end[sb_temp])) {
                                ra_add_node_interference(c->g, node_i, sb_node);
                        }
                }
        }

        c->disable_ldunif_opt = had_disable_ldunif_opt;
        c->spilling = false;
}

struct v3d_ra_select_callback_data {
        uint32_t next_acc;
        uint32_t next_phys;
        struct v3d_ra_node_info *nodes;
};

/* Choosing accumulators improves chances of merging QPU instructions
 * due to these merges requiring that at most 2 rf registers are used
 * by the add and mul instructions.
 */
static bool
v3d_ra_favor_accum(struct v3d_ra_select_callback_data *v3d_ra,
                   BITSET_WORD *regs,
                   int priority)
{
        /* Favor accumulators if we have less that this number of physical
         * registers. Accumulators have more restrictions (like being
         * invalidated through thrsw), so running out of physical registers
         * even if we have accumulators available can lead to register
         * allocation failures.
         */
        static const int available_rf_threshold = 5;
        int available_rf = 0 ;
        for (int i = 0; i < PHYS_COUNT; i++) {
                if (BITSET_TEST(regs, PHYS_INDEX + i))
                        available_rf++;
                if (available_rf >= available_rf_threshold)
                        break;
        }
        if (available_rf < available_rf_threshold)
                return true;

        /* Favor accumulators for short-lived temps (our priority represents
         * liveness), to prevent long-lived temps from grabbing accumulators
         * and preventing follow-up instructions from using them, potentially
         * leading to large portions of the shader being unable to use
         * accumulators and therefore merge instructions successfully.
         */
        static const int priority_threshold = 20;
        if (priority <= priority_threshold)
                return true;

        return false;
}

static bool
v3d_ra_select_accum(struct v3d_ra_select_callback_data *v3d_ra,
                    BITSET_WORD *regs,
                    unsigned int *out)
{
        /* Choose r5 for our ldunifs if possible (nobody else can load to that
         * reg, and it keeps the QPU cond field free from being occupied by
         * ldunifrf).
         */
        int r5 = ACC_INDEX + 5;
        if (BITSET_TEST(regs, r5)) {
                *out = r5;
                return true;
        }

        /* Round-robin through our accumulators to give post-RA instruction
         * selection more options.
         */
        for (int i = 0; i < ACC_COUNT; i++) {
                int acc_off = (v3d_ra->next_acc + i) % ACC_COUNT;
                int acc = ACC_INDEX + acc_off;

                if (BITSET_TEST(regs, acc)) {
                        v3d_ra->next_acc = acc_off + 1;
                        *out = acc;
                        return true;
                }
        }

        return false;
}

static bool
v3d_ra_select_rf(struct v3d_ra_select_callback_data *v3d_ra,
                 BITSET_WORD *regs,
                 unsigned int *out)
{
        for (int i = 0; i < PHYS_COUNT; i++) {
                int phys_off = (v3d_ra->next_phys + i) % PHYS_COUNT;
                int phys = PHYS_INDEX + phys_off;

                if (BITSET_TEST(regs, phys)) {
                        v3d_ra->next_phys = phys_off + 1;
                        *out = phys;
                        return true;
                }
        }

        return false;
}

static unsigned int
v3d_ra_select_callback(unsigned int n, BITSET_WORD *regs, void *data)
{
        struct v3d_ra_select_callback_data *v3d_ra = data;

        unsigned int reg;
        if (v3d_ra_favor_accum(v3d_ra, regs, v3d_ra->nodes->info[n].priority) &&
            v3d_ra_select_accum(v3d_ra, regs, &reg)) {
                return reg;
        }

        if (v3d_ra_select_rf(v3d_ra, regs, &reg))
                return reg;

        /* If we ran out of physical registers try to assign an accumulator
         * if we didn't favor that option earlier.
         */
        if (v3d_ra_select_accum(v3d_ra, regs, &reg))
                return reg;

        unreachable("RA must pass us at least one possible reg.");
}

bool
vir_init_reg_sets(struct v3d_compiler *compiler)
{
        /* Allocate up to 3 regfile classes, for the ways the physical
         * register file can be divided up for fragment shader threading.
         */
        int max_thread_index = (compiler->devinfo->ver >= 40 ? 2 : 3);

        compiler->regs = ra_alloc_reg_set(compiler, PHYS_INDEX + PHYS_COUNT,
                                          false);
        if (!compiler->regs)
                return false;

        for (int threads = 0; threads < max_thread_index; threads++) {
                compiler->reg_class_any[threads] =
                        ra_alloc_contig_reg_class(compiler->regs, 1);
                compiler->reg_class_r5[threads] =
                        ra_alloc_contig_reg_class(compiler->regs, 1);
                compiler->reg_class_phys_or_acc[threads] =
                        ra_alloc_contig_reg_class(compiler->regs, 1);
                compiler->reg_class_phys[threads] =
                        ra_alloc_contig_reg_class(compiler->regs, 1);

                for (int i = PHYS_INDEX;
                     i < PHYS_INDEX + (PHYS_COUNT >> threads); i++) {
                        ra_class_add_reg(compiler->reg_class_phys_or_acc[threads], i);
                        ra_class_add_reg(compiler->reg_class_phys[threads], i);
                        ra_class_add_reg(compiler->reg_class_any[threads], i);
                }

                for (int i = ACC_INDEX + 0; i < ACC_INDEX + ACC_COUNT - 1; i++) {
                        ra_class_add_reg(compiler->reg_class_phys_or_acc[threads], i);
                        ra_class_add_reg(compiler->reg_class_any[threads], i);
                }
                /* r5 can only store a single 32-bit value, so not much can
                 * use it.
                 */
                ra_class_add_reg(compiler->reg_class_r5[threads],
                                 ACC_INDEX + 5);
                ra_class_add_reg(compiler->reg_class_any[threads],
                                 ACC_INDEX + 5);
        }

        ra_set_finalize(compiler->regs, NULL);

        return true;
}

static inline bool
tmu_spilling_allowed(struct v3d_compile *c)
{
        return c->spills + c->fills < c->max_tmu_spills;
}

static void
update_graph_and_reg_classes_for_inst(struct v3d_compile *c, int *acc_nodes,
                                      struct qinst *inst)
{
        int32_t ip = inst->ip;
        assert(ip >= 0);

        /* If the instruction writes r3/r4 (and optionally moves its
         * result to a temp), nothing else can be stored in r3/r4 across
         * it.
         */
        if (vir_writes_r3(c->devinfo, inst)) {
                for (int i = 0; i < c->num_temps; i++) {
                        if (c->temp_start[i] < ip && c->temp_end[i] > ip) {
                                ra_add_node_interference(c->g,
                                                         temp_to_node(i),
                                                         acc_nodes[3]);
                        }
                }
        }

        if (vir_writes_r4(c->devinfo, inst)) {
                for (int i = 0; i < c->num_temps; i++) {
                        if (c->temp_start[i] < ip && c->temp_end[i] > ip) {
                                ra_add_node_interference(c->g,
                                                         temp_to_node(i),
                                                         acc_nodes[4]);
                        }
                }
        }

        if (inst->qpu.type == V3D_QPU_INSTR_TYPE_ALU) {
                switch (inst->qpu.alu.add.op) {
                case V3D_QPU_A_LDVPMV_IN:
                case V3D_QPU_A_LDVPMV_OUT:
                case V3D_QPU_A_LDVPMD_IN:
                case V3D_QPU_A_LDVPMD_OUT:
                case V3D_QPU_A_LDVPMP:
                case V3D_QPU_A_LDVPMG_IN:
                case V3D_QPU_A_LDVPMG_OUT: {
                        /* LDVPMs only store to temps (the MA flag
                         * decides whether the LDVPM is in or out)
                         */
                        assert(inst->dst.file == QFILE_TEMP);
                        set_temp_class_bits(&c->nodes, inst->dst.index,
                                            CLASS_BITS_PHYS);
                        break;
                }

                case V3D_QPU_A_RECIP:
                case V3D_QPU_A_RSQRT:
                case V3D_QPU_A_EXP:
                case V3D_QPU_A_LOG:
                case V3D_QPU_A_SIN:
                case V3D_QPU_A_RSQRT2: {
                        /* The SFU instructions write directly to the
                         * phys regfile.
                         */
                        assert(inst->dst.file == QFILE_TEMP);
                        set_temp_class_bits(&c->nodes, inst->dst.index,
                                            CLASS_BITS_PHYS);
                        break;
                }

                default:
                        break;
                }
        }

        if (inst->src[0].file == QFILE_REG) {
                switch (inst->src[0].index) {
                case 0:
                case 1:
                case 2:
                case 3: {
                        /* Payload setup instructions: Force allocate
                         * the dst to the given register (so the MOV
                         * will disappear).
                         */
                        assert(inst->qpu.alu.mul.op == V3D_QPU_M_MOV);
                        assert(inst->dst.file == QFILE_TEMP);
                        uint32_t node = temp_to_node(inst->dst.index);
                        ra_set_node_reg(c->g, node,
                                        PHYS_INDEX + inst->src[0].index);
                        break;
                }
                }
        }

        if (inst->dst.file == QFILE_TEMP) {
                /* Only a ldunif gets to write to R5, which only has a
                 * single 32-bit channel of storage.
                 *
                 * NOTE: ldunifa is subject to the same, however, going by
                 * shader-db it is best to keep r5 exclusive to ldunif, probably
                 * because ldunif has usually a shorter lifespan, allowing for
                 * more accumulator reuse and QPU merges.
                 */
                if (!inst->qpu.sig.ldunif) {
                        uint8_t class_bits =
                                get_temp_class_bits(&c->nodes, inst->dst.index) &
                                ~CLASS_BITS_R5;
                        set_temp_class_bits(&c->nodes, inst->dst.index,
                                            class_bits);

                } else {
                        /* Until V3D 4.x, we could only load a uniform
                         * to r5, so we'll need to spill if uniform
                         * loads interfere with each other.
                         */
                        if (c->devinfo->ver < 40) {
                                set_temp_class_bits(&c->nodes, inst->dst.index,
                                                    CLASS_BITS_R5);
                        }
                }
        }

        /* All accumulators are invalidated across a thread switch. */
        if (inst->qpu.sig.thrsw) {
                for (int i = 0; i < c->num_temps; i++) {
                        if (c->temp_start[i] < ip && c->temp_end[i] > ip) {
                                set_temp_class_bits(&c->nodes, i,
                                                    CLASS_BITS_PHYS);
                        }
                }
        }
}

/**
 * Returns a mapping from QFILE_TEMP indices to struct qpu_regs.
 *
 * The return value should be freed by the caller.
 */
struct qpu_reg *
v3d_register_allocate(struct v3d_compile *c)
{
        int acc_nodes[ACC_COUNT];
        c->nodes = (struct v3d_ra_node_info) {
                .alloc_count = c->num_temps,
                .info = ralloc_array_size(c, sizeof(c->nodes.info[0]),
                                          c->num_temps + ACC_COUNT),
        };

        struct v3d_ra_select_callback_data callback_data = {
                .next_acc = 0,
                /* Start at RF3, to try to keep the TLB writes from using
                 * RF0-2.
                 */
                .next_phys = 3,
                .nodes = &c->nodes,
        };

        vir_calculate_live_intervals(c);

        /* Convert 1, 2, 4 threads to 0, 1, 2 index.
         *
         * V3D 4.x has double the physical register space, so 64 physical regs
         * are available at both 1x and 2x threading, and 4x has 32.
         */
        c->thread_index = ffs(c->threads) - 1;
        if (c->devinfo->ver >= 40) {
                if (c->thread_index >= 1)
                        c->thread_index--;
        }

        c->g = ra_alloc_interference_graph(c->compiler->regs,
                                           c->num_temps + ARRAY_SIZE(acc_nodes));
        ra_set_select_reg_callback(c->g, v3d_ra_select_callback, &callback_data);

        /* Make some fixed nodes for the accumulators, which we will need to
         * interfere with when ops have implied r3/r4 writes or for the thread
         * switches.  We could represent these as classes for the nodes to
         * live in, but the classes take up a lot of memory to set up, so we
         * don't want to make too many.
         */
        for (uint32_t i = 0; i < ACC_COUNT + c->num_temps; i++) {
                if (i < ACC_COUNT) {
                        acc_nodes[i] = i;
                        ra_set_node_reg(c->g, acc_nodes[i], ACC_INDEX + i);
                        c->nodes.info[i].priority = 0;
                        c->nodes.info[i].class_bits = 0;
                } else {
                        uint32_t t = node_to_temp(i);
                        c->nodes.info[i].priority =
                                c->temp_end[t] - c->temp_start[t];
                        c->nodes.info[i].class_bits = CLASS_BITS_ANY;
                }
        }

        /* Walk the instructions adding register class restrictions and
         * interferences.
         */
        int ip = 0;
        vir_for_each_inst_inorder(inst, c) {
                inst->ip = ip++;
                update_graph_and_reg_classes_for_inst(c, acc_nodes, inst);
        }

        /* Set the register classes for all our temporaries in the graph */
        for (uint32_t i = 0; i < c->num_temps; i++) {
                ra_set_node_class(c->g, temp_to_node(i),
                                  choose_reg_class_for_temp(c, i));
        }

        /* Add register interferences based on liveness data */
        for (uint32_t i = 0; i < c->num_temps; i++) {
                for (uint32_t j = i + 1; j < c->num_temps; j++) {
                        if (interferes(c->temp_start[i], c->temp_end[i],
                                       c->temp_start[j], c->temp_end[j])) {
                                ra_add_node_interference(c->g,
                                                         temp_to_node(i),
                                                         temp_to_node(j));
                        }
                }
        }

        /* Debug option to force a bit of TMU spilling, for running
         * across conformance tests to make sure that spilling works.
         */
        const int force_register_spills = 0;
        if (force_register_spills > 0)
                c->max_tmu_spills = UINT32_MAX;

        struct qpu_reg *temp_registers = NULL;
        while (true) {
                if (c->spill_size <
                    V3D_CHANNELS * sizeof(uint32_t) * force_register_spills) {
                        int node = v3d_choose_spill_node(c);
                        uint32_t temp = node_to_temp(node);
                        if (node != -1) {
                                v3d_spill_reg(c, acc_nodes, temp);
                                continue;
                        }
                }

                if (ra_allocate(c->g))
                        break;

                /* Failed allocation, try to spill */
                int node = v3d_choose_spill_node(c);
                if (node == -1)
                        goto spill_fail;

                uint32_t temp = node_to_temp(node);
                enum temp_spill_type spill_type =
                        get_spill_type_for_temp(c, temp);
                if (spill_type != SPILL_TYPE_TMU || tmu_spilling_allowed(c)) {
                        v3d_spill_reg(c, acc_nodes, temp);
                        if (c->spills + c->fills > c->max_tmu_spills)
                                goto spill_fail;
                } else {
                        goto spill_fail;
                }
        }

        /* Allocation was successful, build the 'temp -> reg' map */
        temp_registers = calloc(c->num_temps, sizeof(*temp_registers));
        for (uint32_t i = 0; i < c->num_temps; i++) {
                int ra_reg = ra_get_node_reg(c->g, temp_to_node(i));
                if (ra_reg < PHYS_INDEX) {
                        temp_registers[i].magic = true;
                        temp_registers[i].index = (V3D_QPU_WADDR_R0 +
                                                   ra_reg - ACC_INDEX);
                } else {
                        temp_registers[i].magic = false;
                        temp_registers[i].index = ra_reg - PHYS_INDEX;
                }
        }

spill_fail:
        ralloc_free(c->nodes.info);
        c->nodes.info = NULL;
        c->nodes.alloc_count = 0;
        ralloc_free(c->g);
        c->g = NULL;
        return temp_registers;
}
