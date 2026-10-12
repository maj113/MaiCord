; First-party x64 compiler support. Memory operations use AVX1 only, matching
; MaiCord's /arch:AVX contract. All small-path loads precede stores, so memcpy
; and memmove can share the overlap-safe implementation without reading beyond
; the caller's range.

OPTION CASEMAP:NONE

PUBLIC _fltused
_fltused EQU 0

_TEXT SEGMENT

PUBLIC memcpy
PUBLIC memmove
memcpy LABEL NEAR
memmove PROC
    mov rax, rcx
    cmp r8, 128
    jbe move_small

    cmp rcx, rdx
    jbe move_forward
    lea r9, [rdx+r8]
    cmp rcx, r9
    jb move_backward

move_forward:
    cmp r8, 192
    jb move_forward_remainder
    vmovdqu ymm0, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rdx+32]
    vmovdqu ymm2, YMMWORD PTR [rdx+64]
    vmovdqu ymm3, YMMWORD PTR [rdx+96]
    vmovdqu ymm4, YMMWORD PTR [rdx+128]
    vmovdqu ymm5, YMMWORD PTR [rdx+160]
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm1
    vmovdqu YMMWORD PTR [rcx+64], ymm2
    vmovdqu YMMWORD PTR [rcx+96], ymm3
    vmovdqu YMMWORD PTR [rcx+128], ymm4
    vmovdqu YMMWORD PTR [rcx+160], ymm5
    add rcx, 192
    add rdx, 192
    sub r8, 192
    cmp r8, 192
    jae move_forward
move_forward_remainder:
    cmp r8, 128
    jbe move_forward_tail
    vmovdqu ymm0, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rdx+32]
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm1
    add rcx, 64
    add rdx, 64
    sub r8, 64
move_forward_tail:
    test r8, r8
    jz move_avx_done
    cmp r8, 32
    ja move_small
    vzeroupper
    jmp move_small

move_backward:
    add rcx, r8
    add rdx, r8
    cmp r8, 192
    jb move_backward_remainder
move_backward_loop:
    sub rcx, 192
    sub rdx, 192
    vmovdqu ymm0, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rdx+32]
    vmovdqu ymm2, YMMWORD PTR [rdx+64]
    vmovdqu ymm3, YMMWORD PTR [rdx+96]
    vmovdqu ymm4, YMMWORD PTR [rdx+128]
    vmovdqu ymm5, YMMWORD PTR [rdx+160]
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm1
    vmovdqu YMMWORD PTR [rcx+64], ymm2
    vmovdqu YMMWORD PTR [rcx+96], ymm3
    vmovdqu YMMWORD PTR [rcx+128], ymm4
    vmovdqu YMMWORD PTR [rcx+160], ymm5
    sub r8, 192
    cmp r8, 192
    jae move_backward_loop
move_backward_remainder:
    cmp r8, 128
    jbe move_backward_tail
    sub rcx, 64
    sub rdx, 64
    vmovdqu ymm0, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rdx+32]
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm1
    sub r8, 64
move_backward_tail:
    sub rcx, r8
    sub rdx, r8
    test r8, r8
    jz move_avx_done
    cmp r8, 32
    ja move_small
    vzeroupper
    jmp move_small

move_avx_done:
    vzeroupper
    ret

move_small:
    cmp r8, 16
    jbe move_up_to_16
    cmp r8, 32
    jbe move_up_to_32
    cmp r8, 64
    jbe move_up_to_64
    vmovdqu ymm0, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rdx+32]
    vmovdqu ymm2, YMMWORD PTR [rdx+r8-64]
    vmovdqu ymm3, YMMWORD PTR [rdx+r8-32]
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm1
    vmovdqu YMMWORD PTR [rcx+r8-64], ymm2
    vmovdqu YMMWORD PTR [rcx+r8-32], ymm3
    vzeroupper
    ret

move_up_to_64:
    vmovdqu ymm0, YMMWORD PTR [rdx]
    vmovdqu ymm1, YMMWORD PTR [rdx+r8-32]
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+r8-32], ymm1
    vzeroupper
    ret

move_up_to_32:
    movdqu xmm0, XMMWORD PTR [rdx]
    movdqu xmm1, XMMWORD PTR [rdx+r8-16]
    movdqu XMMWORD PTR [rcx], xmm0
    movdqu XMMWORD PTR [rcx+r8-16], xmm1
    ret

move_up_to_16:
    cmp r8, 8
    ja move_9_to_16
    cmp r8, 4
    ja move_5_to_8
    cmp r8, 1
    ja move_2_to_4
    jb move_done
    mov r9b, BYTE PTR [rdx]
    mov BYTE PTR [rcx], r9b
move_done:
    ret

move_2_to_4:
    mov r9w, WORD PTR [rdx]
    mov r10w, WORD PTR [rdx+r8-2]
    mov WORD PTR [rcx], r9w
    mov WORD PTR [rcx+r8-2], r10w
    ret

move_5_to_8:
    mov r9d, DWORD PTR [rdx]
    mov r10d, DWORD PTR [rdx+r8-4]
    mov DWORD PTR [rcx], r9d
    mov DWORD PTR [rcx+r8-4], r10d
    ret

move_9_to_16:
    mov r9, QWORD PTR [rdx]
    mov r10, QWORD PTR [rdx+r8-8]
    mov QWORD PTR [rcx], r9
    mov QWORD PTR [rcx+r8-8], r10
    ret
memmove ENDP

PUBLIC memset
memset PROC
    mov rax, rcx
set_small_entry:
    test r8, r8
    jz set_done
    mov BYTE PTR [rcx], dl
    cmp r8, 1
    je set_done
    movzx edx, dl
    imul edx, edx, 01010101h
    cmp r8, 4
    ja set_above_4
    mov WORD PTR [rcx], dx
    mov WORD PTR [rcx+r8-2], dx
    ret

set_above_4:
    mov r9d, edx
    shl r9, 32
    or r9, rdx
    cmp r8, 8
    ja set_above_8
    mov DWORD PTR [rcx], edx
    mov DWORD PTR [rcx+r8-4], edx
    ret

set_above_8:
    cmp r8, 16
    ja set_above_16
    mov QWORD PTR [rcx], r9
    mov QWORD PTR [rcx+r8-8], r9
    ret

set_above_16:
    vmovd xmm0, edx
    vpshufd xmm0, xmm0, 0
    cmp r8, 32
    ja set_above_32
    vmovdqu XMMWORD PTR [rcx], xmm0
    vmovdqu XMMWORD PTR [rcx+r8-16], xmm0
    ret

set_above_32:
    vinsertf128 ymm0, ymm0, xmm0, 1
    cmp r8, 64
    ja set_above_64
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+r8-32], ymm0
    vzeroupper
    ret

set_above_64:
    cmp r8, 128
    ja set_loop
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm0
    vmovdqu YMMWORD PTR [rcx+r8-64], ymm0
    vmovdqu YMMWORD PTR [rcx+r8-32], ymm0
    vzeroupper
    ret

set_loop:
    cmp r8, 192
    jb set_loop_remainder
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm0
    vmovdqu YMMWORD PTR [rcx+64], ymm0
    vmovdqu YMMWORD PTR [rcx+96], ymm0
    vmovdqu YMMWORD PTR [rcx+128], ymm0
    vmovdqu YMMWORD PTR [rcx+160], ymm0
    add rcx, 192
    sub r8, 192
    cmp r8, 192
    jae set_loop
set_loop_remainder:
    cmp r8, 128
    jbe set_loop_tail
    vmovdqu YMMWORD PTR [rcx], ymm0
    vmovdqu YMMWORD PTR [rcx+32], ymm0
    add rcx, 64
    sub r8, 64
set_loop_tail:
    test r8, r8
    jz set_avx_done
    cmp r8, 32
    ja set_small_entry
    vzeroupper
    jmp set_small_entry
set_avx_done:
    vzeroupper
set_done:
    ret
memset ENDP

; Probe every intervening stack page before a large compiler-generated frame.
; Entry: RAX = requested frame size. Exit: RAX and all GPRs preserved; flags are
; clobbered. The caller performs the actual RSP subtraction after this returns.
PUBLIC __chkstk
__chkstk PROC
    push rcx
    push rax
    lea rcx, [rsp+24]
probe_page:
    cmp rax, 1000h
    jb probe_tail
    sub rcx, 1000h
    test BYTE PTR [rcx], 0
    sub rax, 1000h
    jmp probe_page
probe_tail:
    sub rcx, rax
    test BYTE PTR [rcx], 0
    pop rax
    pop rcx
    ret
__chkstk ENDP

_TEXT ENDS
END
