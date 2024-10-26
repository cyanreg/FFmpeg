/*
 * Copyright (c) Lynne
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "asm.S"

#Indices used in REVTAB
double_up_ladder:
    .dword 0x0, 0x0, 0x1, 0x1
    .dword 0x2, 0x2, 0x3, 0x3
    .dword 0x4, 0x4, 0x5, 0x5
    .dword 0x6, 0x6, 0x7, 0x7
    .dword 0x8, 0x8, 0x9, 0x9
    .dword 0xa, 0xa, 0xb, 0xb
    .dword 0xc, 0xc, 0xd, 0xd
    .dword 0xe, 0xe, 0xf, 0xf

zero_eight_repeating:
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8
    .dword 0x0, 0x8, 0x0, 0x8

fft8_shufs_end:
    .dword 0x00, 0x08, 0x20, 0x28
    .dword 0x40, 0x48, 0x60, 0x68
    .dword 0x10, 0x18, 0x30, 0x38
    .dword 0x50, 0x58, 0x70, 0x78

#Load part of complex and permute it with revtab
# a0 - pointer to AVTXContext
# v0 - permuted vector
# n  - number of samples
.macro PERMUTE_REVTAB_I n
    vsetivli t0, \n, e32, m4, ta, ma
    lw t1, 56(a0)                       #*revtab
    vle32.v v0, 0(t1)                   # revtab
    li t1, 16
    vwmul.vx v8, v0, t1                 #multiply revtab indices by 16 (building byte offsets)
    vsetivli t0, \n*2, e64, m8, ta, ma
    la t1, double_up_ladder
    vle64.v v0, (t1)
    vrgather.vv v16, v8, v0              #spread revtab over two doubles for re+im
    la t1, zero_eight_repeating
    vle64.v v8, (t1)
    vfadd.vv v16, v16, v8                 #increment every 2nd offset
    vloxei64.v v0, (a2), v16             #permuted vector
.endm

#Load part of complex and permute it with revtab
# a0 - pointer to AVTXContext
# v0 - permuted vector
# ar - register describing the number of samples
.macro PERMUTE_REVTAB_R ar
    vsetvli t0, \ar, e32, m4, ta, ma
    lw t1, 56(a0)                       #*revtab
    vle32.v v0, 0(t1)                   # revtab
    li t1, 16
    vwmul.vx v8, v0, t1                 #multiply revtab indices by 16 (building byte offsets)
    slli \ar, \ar, 1
    vsetvli t0, \ar, e64, m8, ta, ma
    la t1, double_up_ladder
    vle64.v v0, (t1)
    vrgather.vv v16, v8, v0              #spread revtab over two doubles for re+im
    la t1, zero_eight_repeating
    vle64.v v8, (t1)
    vfadd.vv v16, v16, v8                 #increment every 2nd offset
    vloxei64.v v0, (a2), v16             #permuted vector
.endm

# av0 - source/output vector
# av2, av4, av6, av8 - temp vectors
# at0 - temp scalar
.macro FFT4 av0, av2, av4, av6, av8, at0
    vsetivli        \at0, 8, e64, m2, ta, ma        #16 registers of 8 doubles each
    vslidedown.vi   \av2, \av0, 4                   #4567
    vsetivli        \at0, 4, e64, m2, ta, ma        #16 registers of 4 doubles each
    vfsub.vv        \av4, \av0, \av2                #r1234
    vfadd.vv        \av2, \av0, \av2                #t1234
    vsetivli        \at0, 8, e64, m2, ta, ma        #16 registers of 8 doubles each
    vslideup.vi     \av4, \av2, 4                   #r1234 t1234
    la              \at0, fft4_shufs                #Load shufs
    vle64.v         \av6, 0(\at0)
    addi            \at0, \at0, 64
    vle64.v         \av8, 0(\at0)
    vrgather.vv     \av0, \av4, \av6                #t12r12 t12r12
    vrgather.vv     \av2, \av4, \av8                #t34r43 t34r43
    addi            \at0, \at0, 64                  #Load 'shuf'
    vle64.v         \av8, 0(\at0)
    vfmul.vv        \av2, \av2, \av8                #v8 - PPPN NNNP
    vfadd.vv        \av0, \av0, \av2                #v0 - Final result
.endm

# a0- AVTXContext
# a1- FFTComplex out
# a2- FFTComplex in
# a4- tmp
func ff_fft4_fwd_float_riscv, zve32f, zba
    #PERMUTE_REVTAB_I 4                             #Load array through revtab (unnecessary as it's just 0123)
    vsetivli        t0, 8, e64, m2, ta, ma          #16 registers of 8 doubles each
    vle64.v         v0, (a2)                        #Load array directly

    FFT4 v0, v2, v4, v6, v8, t0                     #FFT4

    #Arrange samples in final order
    la t0,          fft4_shufs_end
    vle64.v         v2, 0(t0)
    vsoxei64.v      v0, (a1), v2
    ret
