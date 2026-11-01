	.def	@feat.00;
	.scl	3;
	.type	0;
	.endef
	.globl	@feat.00
@feat.00 = 0
	.intel_syntax noprefix
	.file	"d3d11_renderer_draw.cpp"
	.def	"?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z",unique,0
	.globl	"?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z" # -- Begin function ?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z
	.p2align	4
"?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z": # @"?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z"
.seh_proc "?renderer_release_settings_backdrop@maicord@@YAXPEAUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	test	rcx, rcx
	je	.LBB0_2
# %bb.1:
	mov	rsi, rcx
	call	"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z"
	vxorps	xmm0, xmm0, xmm0
	vmovups	ymmword ptr [rsi + 99784], ymm0
	mov	qword ptr [rsi + 99816], 0
	mov	dword ptr [rsi + 99828], 0
	mov	byte ptr [rsi + 120314], 0
.LBB0_2:
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	vzeroupper
	ret
	.seh_endproc
                                        # -- End function
	.def	"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z",unique,1
	.p2align	4                               # -- Begin function ?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z
"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z": # @"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z"
.seh_proc "?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	add	rcx, 99760
	call	"?com_release_slot@maicord@@YAXPEAPEAX@Z"
	lea	rcx, [rsi + 99752]
	call	"?com_release_slot@maicord@@YAXPEAPEAX@Z"
	lea	rcx, [rsi + 99744]
	call	"?com_release_slot@maicord@@YAXPEAPEAX@Z"
	mov	byte ptr [rsi + 120312], 0
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z",unique,2
	.globl	"?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z" # -- Begin function ?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z
	.p2align	4
"?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z": # @"?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z"
.seh_proc "?renderer_pin_settings_backdrop_media@maicord@@YAXPEAUD3D11Renderer@1@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 120
	.seh_stackalloc 120
	vmovaps	xmmword ptr [rsp + 96], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 96
	.seh_endprologue
	test	rcx, rcx
	je	.LBB2_12
# %bb.1:
	mov	rsi, rcx
	cmp	byte ptr [rcx + 120312], 1
	jne	.LBB2_12
# %bb.2:
	mov	rcx, rsi
	call	"?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	test	al, al
	je	.LBB2_12
# %bb.3:
	mov	eax, dword ptr [rsi + 99828]
	test	eax, eax
	je	.LBB2_12
# %bb.4:
	cmp	eax, 4096
	mov	r15d, 4096
	cmovb	r15d, eax
	lea	r12, [rsi + 52200]
	lea	r13, [rsi + 52192]
	xor	ebp, ebp
	vxorps	xmm6, xmm6, xmm6
	lea	rbx, [rsp + 64]
	lea	r14, [rsp + 32]
	jmp	.LBB2_5
	.p2align	4
.LBB2_11:                               #   in Loop: Header=BB2_5 Depth=1
	inc	rbp
	cmp	r15, rbp
	je	.LBB2_12
.LBB2_5:                                # =>This Inner Loop Header: Depth=1
	movzx	edi, byte ptr [rsi + rbp + 116216]
	mov	rax, r13
	test	dil, dil
	je	.LBB2_7
# %bb.6:                                #   in Loop: Header=BB2_5 Depth=1
	mov	rax, r12
	cmp	dil, 1
	jne	.LBB2_11
.LBB2_7:                                #   in Loop: Header=BB2_5 Depth=1
	mov	rcx, qword ptr [rax]
	vmovaps	xmmword ptr [rsp + 64], xmm6
	mov	dword ptr [rsp + 80], 0
	test	rcx, rcx
	je	.LBB2_11
# %bb.8:                                #   in Loop: Header=BB2_5 Depth=1
	mov	edx, dword ptr [rsi + 4*rbp + 99832]
	mov	r8, rbx
	call	"?media_atlas_resolve_visible@maicord@@YA_NPEAUMediaAtlas@1@UMediaAtlasHandle@1@PEAUMediaAtlasResult@1@@Z"
	test	al, al
	je	.LBB2_11
# %bb.9:                                #   in Loop: Header=BB2_5 Depth=1
	vmovaps	xmmword ptr [rsp + 32], xmm6
	mov	dword ptr [rsp + 48], 0
	test	dil, dil
	jne	.LBB2_11
# %bb.10:                               #   in Loop: Header=BB2_5 Depth=1
	lea	rcx, [rsi + 52944]
	mov	rdx, rbx
	mov	r8, r14
	call	"?animation_frame_cache_gpu_resolve@maicord@@YA_NPEAUAnimationFrameCacheGpu@1@AEBUMediaAtlasResult@1@PEAUGlyphAtlasAllocation@1@@Z"
	jmp	.LBB2_11
.LBB2_12:
	vmovaps	xmm6, xmmword ptr [rsp + 96]    # 16-byte Reload
	.seh_startepilogue
	add	rsp, 120
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z",unique,3
	.p2align	4                               # -- Begin function ?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z
"?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z": # @"?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
.seh_proc "?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB3_2
# %bb.1:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB3_2:
	mov	rsi, rcx
	call	"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	mov	ecx, eax
	mov	al, 1
	test	cl, cl
	je	.LBB3_4
# %bb.3:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB3_4:
	mov	rcx, rsi
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	jmp	"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z" # TAILCALL
	.seh_endproc
                                        # -- End function
	.def	"?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z",unique,4
	.globl	"?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z" # -- Begin function ?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z
	.p2align	4
"?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z": # @"?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
.seh_proc "?renderer_freeze_login_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 48
	.seh_stackalloc 48
	.seh_endprologue
	test	rcx, rcx
	je	.LBB4_10
# %bb.1:
	cmp	qword ptr [rcx + 56], 0
	je	.LBB4_10
# %bb.2:
	cmp	byte ptr [rcx + 120537], 1
	jne	.LBB4_10
# %bb.3:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	qword ptr [rsp + 40], rdx       # 8-byte Spill
	call	"?d3d11_renderer_build_paint@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
	test	al, al
	je	.LBB4_10
# %bb.4:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	call	"?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z"
	test	al, al
	je	.LBB4_10
# %bb.5:
	mov	rsi, qword ptr [rsp + 32]       # 8-byte Reload
	lea	rdi, [rsi + 56]
	mov	rcx, rsi
	mov	rdx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z"
	mov	ebx, eax
	mov	rcx, qword ptr [rsi + 8]
	mov	rax, qword ptr [rcx]
	mov	edx, 1
	mov	r8, rdi
	xor	r9d, r9d
	call	qword ptr [rax + 264]
	mov	eax, ebx
	mov	dword ptr [rsi + 99828], 0
	mov	byte ptr [rsi + 120314], al
	mov	byte ptr [rsi + 120312], al
	mov	byte ptr [rsi + 82792], 0
	jmp	.LBB4_11
.LBB4_10:
	xor	eax, eax
.LBB4_11:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 48
	pop	rbx
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__xmm@00000000000000000000002800000000 # -- Begin function ?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z
	.section	.rdata,"dr",discard,__xmm@00000000000000000000002800000000
	.p2align	4, 0x0
__xmm@00000000000000000000002800000000:
	.long	0                               # 0x0
	.long	40                              # 0x28
	.long	0                               # 0x0
	.long	0                               # 0x0
	.globl	__xmm@00010000000000010000000200000040
	.section	.rdata,"dr",discard,__xmm@00010000000000010000000200000040
	.p2align	4, 0x0
__xmm@00010000000000010000000200000040:
	.long	64                              # 0x40
	.long	2                               # 0x2
	.long	1                               # 0x1
	.long	65536                           # 0x10000
	.section	.text,"xr",one_only,"?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z",unique,5
	.p2align	4
"?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z": # @"?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z"
.seh_proc "?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 120
	.seh_stackalloc 120
	.seh_endprologue
	mov	rsi, rcx
	cmp	qword ptr [rcx + 99744], 0
	je	.LBB5_3
# %bb.1:
	mov	eax, dword ptr [rsi + 99776]
	cmp	eax, dword ptr [rsi + 104]
	jne	.LBB5_3
# %bb.2:
	mov	ecx, dword ptr [rsi + 99780]
	mov	al, 1
	cmp	ecx, dword ptr [rsi + 108]
	je	.LBB5_21
.LBB5_3:
	mov	rcx, rsi
	call	"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z"
	mov	qword ptr [rsp + 40], 0
	mov	rcx, qword ptr [rsi + 56]
	mov	rax, qword ptr [rcx]
	lea	rdx, [rsp + 40]
	call	qword ptr [rax + 56]
	mov	qword ptr [rsp + 32], 0
	mov	rcx, qword ptr [rsp + 40]
	test	rcx, rcx
	je	.LBB5_19
# %bb.4:
	mov	rax, qword ptr [rcx]
	lea	rdx, [rip + _GUID_6f15aaf2_d208_4e89_9ab4_489535d34f9c]
	lea	r8, [rsp + 32]
	call	qword ptr [rax]
	mov	edi, eax
	vxorps	xmm0, xmm0, xmm0
	vmovups	ymmword ptr [rsp + 60], ymm0
	vmovups	ymmword ptr [rsp + 48], ymm0
	mov	rcx, qword ptr [rsp + 32]
	test	eax, eax
	js	.LBB5_6
# %bb.5:
	mov	rax, qword ptr [rcx]
	lea	rdx, [rsp + 48]
	vzeroupper
	call	qword ptr [rax + 80]
	mov	rcx, qword ptr [rsp + 32]
.LBB5_6:
	test	rcx, rcx
	je	.LBB5_8
# %bb.7:
	mov	rax, qword ptr [rcx]
	vzeroupper
	call	qword ptr [rax + 16]
.LBB5_8:
	mov	rcx, qword ptr [rsp + 40]
	test	rcx, rcx
	je	.LBB5_10
# %bb.9:
	mov	rax, qword ptr [rcx]
	vzeroupper
	call	qword ptr [rax + 16]
.LBB5_10:
	test	edi, edi
	js	.LBB5_20
# %bb.11:
	mov	eax, dword ptr [rsp + 48]
	cmp	eax, dword ptr [rsi + 104]
	jne	.LBB5_20
# %bb.12:
	mov	eax, dword ptr [rsi + 108]
	mov	ecx, dword ptr [rsi + 120524]
	add	ecx, eax
	cmp	dword ptr [rsp + 52], ecx
	jne	.LBB5_20
# %bb.13:
	lea	r9, [rsi + 99744]
	mov	dword ptr [rsp + 52], eax
	vmovsd	xmm0, qword ptr [rip + __xmm@00000000000000000000002800000000] # xmm0 = [0,40,0,0]
	vmovups	xmmword ptr [rsp + 76], xmm0
	movabs	rax, 4294967297
	mov	qword ptr [rsp + 56], rax
	mov	rcx, qword ptr [rsi]
	mov	rax, qword ptr [rcx]
	lea	rdx, [rsp + 48]
	xor	r8d, r8d
	vzeroupper
	call	qword ptr [rax + 40]
	test	eax, eax
	js	.LBB5_20
# %bb.14:
	mov	rcx, qword ptr [rsi]
	lea	r9, [rsi + 99752]
	mov	rdx, qword ptr [rsi + 99744]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 56]
	test	eax, eax
	js	.LBB5_20
# %bb.15:
	mov	rcx, qword ptr [rsi]
	lea	r9, [rsi + 99760]
	mov	rdx, qword ptr [rsi + 99744]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 72]
	test	eax, eax
	js	.LBB5_20
# %bb.16:
	cmp	qword ptr [rsi + 99768], 0
	jne	.LBB5_18
# %bb.17:
	lea	r9, [rsi + 99768]
	mov	qword ptr [rsp + 112], 0
	vmovaps	xmm0, xmmword ptr [rip + __xmm@00010000000000010000000200000040] # xmm0 = [64,2,1,65536]
	vmovaps	xmmword ptr [rsp + 96], xmm0
	mov	rcx, qword ptr [rsi]
	mov	rax, qword ptr [rcx]
	lea	rdx, [rsp + 96]
	xor	r8d, r8d
	call	qword ptr [rax + 24]
	test	eax, eax
	js	.LBB5_20
.LBB5_18:
	mov	rax, qword ptr [rsi + 104]
	mov	qword ptr [rsi + 99776], rax
	mov	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 120
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
.LBB5_19:
	vxorps	xmm0, xmm0, xmm0
	vmovups	ymmword ptr [rsp + 60], ymm0
	vmovups	ymmword ptr [rsp + 48], ymm0
.LBB5_20:
	mov	rcx, rsi
	vzeroupper
	call	"?release_backdrop_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@@Z"
	xor	eax, eax
.LBB5_21:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 120
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z",unique,6
	.p2align	4                               # -- Begin function ?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z
"?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z": # @"?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z"
.seh_proc "?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z"
# %bb.0:
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 72
	.seh_stackalloc 72
	.seh_endprologue
	mov	rbx, rdx
	mov	rsi, rcx
	vmovsd	xmm0, qword ptr [rdx]           # xmm0 = mem[0],zero
	vmovsd	qword ptr [rsp + 56], xmm0
	vmovss	xmm0, dword ptr [rdx + 8]       # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 64], xmm0
	mov	dword ptr [rsp + 68], 1065353216
	mov	r14, qword ptr [rcx + 56]
	mov	edi, dword ptr [rcx + 120524]
	mov	rax, qword ptr [rcx + 99760]
	mov	qword ptr [rcx + 56], rax
	xor	edx, edx
	call	"?renderer_bind_target_origin@maicord@@YAXPEAUD3D11Renderer@1@I@Z"
	mov	rcx, qword ptr [rsi + 8]
	mov	rdx, qword ptr [rsi + 56]
	mov	rax, qword ptr [rcx]
	lea	r8, [rsp + 56]
	call	qword ptr [rax + 400]
	mov	qword ptr [rsp + 40], -1
	mov	qword ptr [rsp + 32], 0
	mov	rcx, rsi
	mov	rdx, rbx
	mov	r8b, 1
	mov	r9b, 1
	call	"?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z"
	mov	ebx, eax
	mov	qword ptr [rsi + 56], r14
	mov	rcx, rsi
	mov	edx, edi
	call	"?renderer_bind_target_origin@maicord@@YAXPEAUD3D11Renderer@1@I@Z"
	mov	eax, ebx
	.seh_startepilogue
	add	rsp, 72
	pop	rbx
	pop	rdi
	pop	rsi
	pop	r14
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z",unique,7
	.globl	"?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z" # -- Begin function ?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z
	.p2align	4
"?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z": # @"?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
.seh_proc "?renderer_settings_backdrop_stale@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 88
	.seh_stackalloc 88
	.seh_endprologue
	test	rcx, rcx
	je	.LBB7_4
# %bb.1:
	cmp	qword ptr [rcx + 56], 0
	je	.LBB7_4
# %bb.2:
	cmp	byte ptr [rcx + 23628], 1
	jne	.LBB7_4
# %bb.3:
	cmp	byte ptr [rcx + 120314], 0
	jne	.LBB7_4
# %bb.9:
	mov	qword ptr [rsp + 40], rdx       # 8-byte Spill
	mov	rsi, rcx
	call	"?settings_backdrop_wanted@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	test	al, al
	je	.LBB7_4
# %bb.10:
	mov	al, 1
	cmp	byte ptr [rsi + 99816], 1
	jne	.LBB7_5
# %bb.11:
	mov	rcx, rsi
	call	"?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	mov	ecx, eax
	mov	al, 1
	test	cl, cl
	jne	.LBB7_5
# %bb.12:
	mov	rdx, rsi
	add	rsi, 99784
	lea	rdi, [rsp + 48]
	mov	rcx, rdi
	mov	r8, qword ptr [rsp + 40]        # 8-byte Reload
	call	"?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z"
	mov	rcx, rdi
	mov	rdx, rsi
	call	"?renderer_presentation_stamp_equal@maicord@@YA_NAEBURendererPresentationStamp@1@0@Z"
                                        # kill: def $al killed $al def $eax
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 88
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
.LBB7_4:
	xor	eax, eax
.LBB7_5:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 88
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z",unique,8
	.p2align	4                               # -- Begin function ?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z
"?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z": # @"?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
.seh_proc "?shell_backdrop_media_changed@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	cmp	dword ptr [rcx + 83824], 0
	je	.LBB8_6
# %bb.1:
	mov	rsi, rcx
	cmp	dword ptr [rcx + 99828], 0
	je	.LBB8_6
# %bb.2:
	lea	rdi, [rsi + 82800]
	xor	ebx, ebx
	.p2align	4
.LBB8_3:                                # =>This Inner Loop Header: Depth=1
	movzx	r8d, byte ptr [rsi + rbx + 116216]
	mov	edx, dword ptr [rsi + 4*rbx + 99832]
	mov	rcx, rdi
	call	"?presentation_damage_media_pending@maicord@@YA_NAEBUPresentationDamageTracker@1@UMediaAtlasHandle@1@E@Z"
                                        # kill: def $al killed $al def $eax
	test	al, al
	jne	.LBB8_7
# %bb.4:                                #   in Loop: Header=BB8_3 Depth=1
	inc	rbx
	mov	ecx, dword ptr [rsi + 99828]
	cmp	rbx, rcx
	jb	.LBB8_3
	jmp	.LBB8_7
.LBB8_6:
	xor	eax, eax
.LBB8_7:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rbx
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z",unique,9
	.p2align	4                               # -- Begin function ?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z
"?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z": # @"?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z"
.seh_proc "?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 104
	.seh_stackalloc 104
	vmovaps	xmmword ptr [rsp + 80], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 80
	.seh_endprologue
	mov	rsi, rdx
	mov	rdi, rcx
	vmovups	xmm6, xmmword ptr [rdx + 63968]
	mov	rax, qword ptr [rdx + 61104]
	mov	qword ptr [rsp + 40], rax       # 8-byte Spill
	mov	r14, qword ptr [rdx + 63984]
	mov	r15, qword ptr [rdx + 64000]
	mov	r12, qword ptr [rdx + 61048]
	mov	r13, qword ptr [rdx + 22736]
	mov	rbp, qword ptr [rdx + 61080]
	vmovups	ymm0, ymmword ptr [rdx + 63880]
	vmovups	ymmword ptr [rsp + 48], ymm0    # 32-byte Spill
	mov	rax, qword ptr [rdx + 88]
	mov	qword ptr [rsp + 32], rax       # 8-byte Spill
	mov	qword ptr [rdx + 61104], 0
	mov	qword ptr [rdx + 64000], 0
	mov	qword ptr [rdx + 61048], 0
	mov	qword ptr [rdx + 22736], 0
	mov	qword ptr [rdx + 61080], 0
	vxorps	xmm0, xmm0, xmm0
	vmovups	ymmword ptr [rdx + 63880], ymm0
	vmovups	xmmword ptr [rdx + 63968], xmm0
	mov	qword ptr [rdx + 63984], 0
	mov	qword ptr [rdx + 88], 0
	xor	ebx, ebx
	xor	r9d, r9d
	vzeroupper
	call	"?renderer_capture_presentation_stamp@maicord@@YA?AURendererPresentationStamp@1@AEBUD3D11Renderer@1@AEBUTheme@1@_N@Z"
	mov	rax, qword ptr [rsi + 68752]
	test	rax, rax
	je	.LBB9_2
# %bb.1:
	mov	rbx, qword ptr [rax + 264]
.LBB9_2:
	mov	rax, qword ptr [rdi]
	xor	rax, rbx
	movabs	rcx, 1099511628211
	imul	rcx, rax
	mov	qword ptr [rdi], rcx
	rol	rbx, 31
	xor	qword ptr [rdi + 8], rbx
	vmovups	xmmword ptr [rsi + 63968], xmm6
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	qword ptr [rsi + 61104], rax
	mov	qword ptr [rsi + 63984], r14
	mov	qword ptr [rsi + 64000], r15
	mov	qword ptr [rsi + 61048], r12
	mov	qword ptr [rsi + 22736], r13
	mov	qword ptr [rsi + 61080], rbp
	vmovups	ymm0, ymmword ptr [rsp + 48]    # 32-byte Reload
	vmovups	ymmword ptr [rsi + 63880], ymm0
	mov	rax, qword ptr [rsp + 32]       # 8-byte Reload
	mov	qword ptr [rsi + 88], rax
	vmovaps	xmm6, xmmword ptr [rsp + 80]    # 16-byte Reload
	.seh_startepilogue
	add	rsp, 104
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	vzeroupper
	ret
	.seh_endproc
                                        # -- End function
	.def	"?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z",unique,10
	.globl	"?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z" # -- Begin function ?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z
	.p2align	4
"?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z": # @"?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
.seh_proc "?renderer_refresh_settings_backdrop@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 200
	.seh_stackalloc 200
	vmovaps	xmmword ptr [rsp + 176], xmm6   # 16-byte Spill
	.seh_savexmm xmm6, 176
	.seh_endprologue
	test	rcx, rcx
	je	.LBB10_18
# %bb.1:
	mov	rsi, rcx
	cmp	qword ptr [rcx + 56], 0
	je	.LBB10_18
# %bb.2:
	vmovups	xmm6, xmmword ptr [rsi + 63968]
	mov	rbx, qword ptr [rsi + 61104]
	mov	rbp, qword ptr [rsi + 63984]
	mov	r13, qword ptr [rsi + 64000]
	mov	r12, qword ptr [rsi + 61048]
	mov	r15, qword ptr [rsi + 22736]
	mov	r14, qword ptr [rsi + 61080]
	vmovups	ymm0, ymmword ptr [rsi + 63880]
	vmovups	ymmword ptr [rsp + 144], ymm0   # 32-byte Spill
	mov	rdi, qword ptr [rsi + 88]
	mov	qword ptr [rsi + 61104], 0
	mov	qword ptr [rsi + 64000], 0
	mov	qword ptr [rsi + 61048], 0
	mov	qword ptr [rsi + 22736], 0
	mov	qword ptr [rsi + 61080], 0
	vxorps	xmm0, xmm0, xmm0
	vmovups	ymmword ptr [rsi + 63880], ymm0
	vmovups	xmmword ptr [rsi + 63968], xmm0
	mov	qword ptr [rsi + 63984], 0
	test	rdi, rdi
	mov	qword ptr [rsp + 48], rdx       # 8-byte Spill
	je	.LBB10_21
# %bb.3:
	mov	rcx, rsi
	mov	r8b, 1
	vzeroupper
	call	"?d3d11_renderer_rebuild_text_mode@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N@Z"
	test	al, al
	jne	.LBB10_5
# %bb.4:
	mov	rcx, rsi
	mov	rdx, qword ptr [rsp + 48]       # 8-byte Reload
	xor	r8d, r8d
	call	"?d3d11_renderer_rebuild_text_mode@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N@Z"
	xor	ecx, ecx
	test	al, al
	je	.LBB10_22
.LBB10_5:
	mov	rcx, rsi
	mov	rdx, qword ptr [rsp + 48]       # 8-byte Reload
	call	"?d3d11_renderer_build_paint@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@@Z"
	test	al, al
	je	.LBB10_21
# %bb.6:
	mov	rcx, rsi
	call	"?create_backdrop_target@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@@Z"
	test	al, al
	je	.LBB10_21
# %bb.7:
	mov	qword ptr [rsp + 96], rdi       # 8-byte Spill
	mov	qword ptr [rsp + 88], r14       # 8-byte Spill
	mov	qword ptr [rsp + 80], r15       # 8-byte Spill
	mov	qword ptr [rsp + 72], r12       # 8-byte Spill
	mov	qword ptr [rsp + 64], r13       # 8-byte Spill
	mov	qword ptr [rsp + 56], rbp       # 8-byte Spill
	lea	r12, [rsi + 56]
	mov	rcx, rsi
	mov	rdx, qword ptr [rsp + 48]       # 8-byte Reload
	call	"?draw_stream_to_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@@Z"
                                        # kill: def $al killed $al def $eax
	test	al, al
	je	.LBB10_16
# %bb.8:
	mov	edi, dword ptr [rsi + 96]
	cmp	edi, 4096
	mov	ecx, 4096
	cmovb	ecx, edi
	test	edi, edi
	je	.LBB10_15
# %bb.9:
	mov	rdx, qword ptr [rsi + 61000]
	mov	r8, qword ptr [rsi + 61008]
	lea	r9, [rsi + 99832]
	lea	r11, [rsi + 116216]
	mov	r14d, ecx
	cmp	edi, 4
	jae	.LBB10_29
# %bb.10:
	xor	r10d, r10d
.LBB10_11:
	mov	rdi, r10
	test	r14b, 1
	je	.LBB10_13
# %bb.12:
	lea	edi, [4*r10]
	mov	r15d, dword ptr [rdx + rdi]
	mov	dword ptr [r9 + rdi], r15d
	movzx	edi, byte ptr [r8 + r10]
	mov	byte ptr [r11 + r10], dil
	lea	rdi, [r10 + 1]
.LBB10_13:
	lea	r11, [r14 - 1]
	cmp	r10, r11
	je	.LBB10_15
.LBB10_14:                              # =>This Inner Loop Header: Depth=1
	mov	r10d, dword ptr [rdx + 4*rdi]
	mov	dword ptr [r9 + 4*rdi], r10d
	movzx	r10d, byte ptr [r8 + rdi]
	mov	byte ptr [r9 + rdi + 16384], r10b
	mov	r10d, dword ptr [rdx + 4*rdi + 4]
	mov	dword ptr [r9 + 4*rdi + 4], r10d
	movzx	r10d, byte ptr [r8 + rdi + 1]
	mov	byte ptr [r9 + rdi + 16385], r10b
	add	rdi, 2
	cmp	r14, rdi
	jne	.LBB10_14
.LBB10_15:
	mov	dword ptr [rsi + 99828], ecx
.LBB10_16:
	mov	rcx, qword ptr [rsi + 8]
	mov	r10, qword ptr [rcx]
	mov	edx, 1
	mov	r8, r12
	xor	r9d, r9d
	mov	edi, eax
	vzeroupper
	call	qword ptr [r10 + 264]
	mov	ecx, edi
	mov	rbp, qword ptr [rsp + 56]       # 8-byte Reload
	mov	r13, qword ptr [rsp + 64]       # 8-byte Reload
	mov	r12, qword ptr [rsp + 72]       # 8-byte Reload
	mov	r15, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r14, qword ptr [rsp + 88]       # 8-byte Reload
	mov	rdi, qword ptr [rsp + 96]       # 8-byte Reload
	jmp	.LBB10_22
.LBB10_18:
	xor	eax, eax
	jmp	.LBB10_28
.LBB10_21:
	xor	ecx, ecx
.LBB10_22:
	vmovups	xmmword ptr [rsi + 63968], xmm6
	mov	qword ptr [rsi + 61104], rbx
	mov	qword ptr [rsi + 63984], rbp
	mov	qword ptr [rsi + 64000], r13
	mov	qword ptr [rsi + 61048], r12
	mov	qword ptr [rsi + 22736], r15
	mov	qword ptr [rsi + 61080], r14
	vmovups	ymm0, ymmword ptr [rsp + 144]   # 32-byte Reload
	vmovups	ymmword ptr [rsi + 63880], ymm0
	mov	qword ptr [rsi + 88], rdi
	test	cl, cl
	je	.LBB10_24
# %bb.23:
	mov	byte ptr [rsi + 120312], 1
	inc	dword ptr [rsi + 99824]
.LBB10_24:
	xor	cl, 1
	mov	byte ptr [rsi + 120313], cl
	mov	byte ptr [rsi + 82792], 0
	lea	rcx, [rsi + 82800]
	vzeroupper
	call	"?presentation_damage_invalidate_full@maicord@@YAXPEAUPresentationDamageTracker@1@@Z"
	cmp	qword ptr [rsi + 88], 0
	je	.LBB10_26
# %bb.25:
	mov	rcx, rsi
	mov	rdi, qword ptr [rsp + 48]       # 8-byte Reload
	mov	rdx, rdi
	mov	r8b, 1
	call	"?d3d11_renderer_rebuild_text_mode@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N@Z"
	mov	ebx, eax
	jmp	.LBB10_27
.LBB10_26:
	mov	bl, 1
	mov	rdi, qword ptr [rsp + 48]       # 8-byte Reload
.LBB10_27:
	lea	rcx, [rsp + 104]
	mov	rdx, rsi
	mov	r8, rdi
	call	"?shell_backdrop_stamp@?A0xF64B795B@maicord@@YA?AURendererPresentationStamp@2@PEAUD3D11Renderer@2@AEBUTheme@2@@Z"
	mov	rax, qword ptr [rsp + 136]
	mov	qword ptr [rsi + 99816], rax
	vmovups	ymm0, ymmword ptr [rsp + 104]
	vmovups	ymmword ptr [rsi + 99784], ymm0
	mov	eax, ebx
.LBB10_28:
                                        # kill: def $al killed $al killed $eax
	vmovaps	xmm6, xmmword ptr [rsp + 176]   # 16-byte Reload
	.seh_startepilogue
	add	rsp, 200
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	vzeroupper
	ret
.LBB10_29:
	lea	r10, [rsi + 4*r14]
	add	r10, 99832
	lea	r15, [rsi + r14]
	add	r15, 116216
	lea	rbp, [rdx + 4*r14]
	lea	r13, [r8 + r14]
	cmp	r9, r15
	setb	byte ptr [rsp + 47]             # 1-byte Folded Spill
	cmp	r11, r10
	setb	byte ptr [rsp + 46]             # 1-byte Folded Spill
	cmp	r9, rbp
	setb	r12b
	cmp	rdx, r10
	setb	byte ptr [rsp + 45]             # 1-byte Folded Spill
	cmp	r9, r13
	setb	r15b
	cmp	r8, r10
	setb	byte ptr [rsp + 44]             # 1-byte Folded Spill
	cmp	r11, rbp
	setb	bpl
	lea	r10, [rsi + r14]
	add	r10, 116216
	cmp	rdx, r10
	setb	byte ptr [rsp + 43]             # 1-byte Folded Spill
	cmp	r11, r13
	setb	byte ptr [rsp + 42]             # 1-byte Folded Spill
	lea	r10, [rsi + r14]
	add	r10, 116216
	cmp	r8, r10
	setb	byte ptr [rsp + 41]             # 1-byte Folded Spill
	xor	r10d, r10d
	movzx	r13d, byte ptr [rsp + 46]       # 1-byte Folded Reload
	test	byte ptr [rsp + 47], r13b       # 1-byte Folded Reload
	jne	.LBB10_44
# %bb.30:
	and	r12b, byte ptr [rsp + 45]       # 1-byte Folded Reload
	jne	.LBB10_44
# %bb.31:
	and	r15b, byte ptr [rsp + 44]       # 1-byte Folded Reload
	lea	r12, [rsi + 56]
	jne	.LBB10_11
# %bb.32:
	and	bpl, byte ptr [rsp + 43]        # 1-byte Folded Reload
	jne	.LBB10_11
# %bb.33:
	movzx	r15d, byte ptr [rsp + 41]       # 1-byte Folded Reload
	and	byte ptr [rsp + 42], r15b       # 1-byte Folded Spill
	jne	.LBB10_11
# %bb.34:
	cmp	edi, 16
	jae	.LBB10_36
# %bb.35:
	xor	r10d, r10d
	jmp	.LBB10_40
.LBB10_36:
	mov	r10d, r14d
	and	r10d, 8176
	xor	edi, edi
.LBB10_37:                              # =>This Inner Loop Header: Depth=1
	vmovups	ymm0, ymmword ptr [rdx + 4*rdi]
	vmovups	ymm1, ymmword ptr [rdx + 4*rdi + 32]
	vmovups	ymmword ptr [rsi + 4*rdi + 99832], ymm0
	vmovups	ymmword ptr [rsi + 4*rdi + 99864], ymm1
	vmovups	xmm0, xmmword ptr [r8 + rdi]
	vmovups	xmmword ptr [rsi + rdi + 116216], xmm0
	add	rdi, 16
	cmp	r10, rdi
	jne	.LBB10_37
# %bb.38:
	cmp	r10d, r14d
	je	.LBB10_15
# %bb.39:
	test	r14b, 12
	je	.LBB10_11
.LBB10_40:
	mov	rdi, r10
	mov	r10d, r14d
	and	r10d, 8188
.LBB10_41:                              # =>This Inner Loop Header: Depth=1
	vmovups	xmm0, xmmword ptr [rdx + 4*rdi]
	vmovups	xmmword ptr [r11 + 4*rdi - 16384], xmm0
	mov	r15d, dword ptr [r8 + rdi]
	mov	dword ptr [r11 + rdi], r15d
	add	rdi, 4
	cmp	r10, rdi
	jne	.LBB10_41
# %bb.42:
	cmp	r10d, r14d
	jne	.LBB10_11
	jmp	.LBB10_15
.LBB10_44:
	lea	r12, [rsi + 56]
	jmp	.LBB10_11
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z";
	.scl	2;
	.type	32;
	.endef
	.globl	__real@42c00000                 # -- Begin function ?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z
	.section	.rdata,"dr",discard,__real@42c00000
	.p2align	2, 0x0
__real@42c00000:
	.long	0x42c00000                      # float 96
	.globl	__real@3f800000
	.section	.rdata,"dr",discard,__real@3f800000
	.p2align	2, 0x0
__real@3f800000:
	.long	0x3f800000                      # float 1
	.globl	__xmm@3f8000003f8000003f8000003f800000
	.section	.rdata,"dr",discard,__xmm@3f8000003f8000003f8000003f800000
	.p2align	4, 0x0
__xmm@3f8000003f8000003f8000003f800000:
	.long	0x3f800000                      # float 1
	.long	0x3f800000                      # float 1
	.long	0x3f800000                      # float 1
	.long	0x3f800000                      # float 1
	.globl	__real@bdf5c28f
	.section	.rdata,"dr",discard,__real@bdf5c28f
	.p2align	2, 0x0
__real@bdf5c28f:
	.long	0xbdf5c28f                      # float -0.119999997
	.globl	__real@3f6147ae
	.section	.rdata,"dr",discard,__real@3f6147ae
	.p2align	2, 0x0
__real@3f6147ae:
	.long	0x3f6147ae                      # float 0.879999995
	.globl	__real@3dcccccd
	.section	.rdata,"dr",discard,__real@3dcccccd
	.p2align	2, 0x0
__real@3dcccccd:
	.long	0x3dcccccd                      # float 0.100000001
	.globl	__real@3f666666
	.section	.rdata,"dr",discard,__real@3f666666
	.p2align	2, 0x0
__real@3f666666:
	.long	0x3f666666                      # float 0.899999976
	.globl	__real@bdcccccd
	.section	.rdata,"dr",discard,__real@bdcccccd
	.p2align	2, 0x0
__real@bdcccccd:
	.long	0xbdcccccd                      # float -0.100000001
	.globl	__real@3f8ccccd
	.section	.rdata,"dr",discard,__real@3f8ccccd
	.p2align	2, 0x0
__real@3f8ccccd:
	.long	0x3f8ccccd                      # float 1.10000002
	.globl	__xmm@00000000000000003f0000003f000000
	.section	.rdata,"dr",discard,__xmm@00000000000000003f0000003f000000
	.p2align	4, 0x0
__xmm@00000000000000003f0000003f000000:
	.long	0x3f000000                      # float 0.5
	.long	0x3f000000                      # float 0.5
	.zero	4
	.zero	4
	.globl	__xmm@00000000000000003f8000003f800000
	.section	.rdata,"dr",discard,__xmm@00000000000000003f8000003f800000
	.p2align	4, 0x0
__xmm@00000000000000003f8000003f800000:
	.long	0x3f800000                      # float 1
	.long	0x3f800000                      # float 1
	.long	0x00000000                      # float 0
	.long	0x00000000                      # float 0
	.section	.text,"xr",one_only,"?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z",unique,11
	.globl	"?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z"
	.p2align	4
"?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z": # @"?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z"
.seh_proc "?d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 1608
	.seh_stackalloc 1608
	vmovaps	xmmword ptr [rsp + 1584], xmm13 # 16-byte Spill
	.seh_savexmm xmm13, 1584
	vmovaps	xmmword ptr [rsp + 1568], xmm12 # 16-byte Spill
	.seh_savexmm xmm12, 1568
	vmovaps	xmmword ptr [rsp + 1552], xmm11 # 16-byte Spill
	.seh_savexmm xmm11, 1552
	vmovaps	xmmword ptr [rsp + 1536], xmm10 # 16-byte Spill
	.seh_savexmm xmm10, 1536
	vmovaps	xmmword ptr [rsp + 1520], xmm9  # 16-byte Spill
	.seh_savexmm xmm9, 1520
	vmovaps	xmmword ptr [rsp + 1504], xmm8  # 16-byte Spill
	.seh_savexmm xmm8, 1504
	vmovaps	xmmword ptr [rsp + 1488], xmm7  # 16-byte Spill
	.seh_savexmm xmm7, 1488
	vmovaps	xmmword ptr [rsp + 1472], xmm6  # 16-byte Spill
	.seh_savexmm xmm6, 1472
	.seh_endprologue
	test	rcx, rcx
	je	.LBB11_5
# %bb.1:
	mov	r10d, dword ptr [rcx + 82728]
	xor	eax, eax
	cmp	r10d, 4096
	ja	.LBB11_6
# %bb.2:
	cmp	r10d, dword ptr [rcx + 82748]
	ja	.LBB11_6
# %bb.3:
	cmp	dword ptr [rcx + 82732], 256
	ja	.LBB11_5
# %bb.7:
	cmp	dword ptr [rcx + 82736], 256
	ja	.LBB11_5
# %bb.9:
	test	r10d, r10d
	je	.LBB11_11
# %bb.10:
	cmp	qword ptr [rcx + 75520], 0
	je	.LBB11_5
.LBB11_11:
	mov	ebx, r8d
	mov	byte ptr [rsp + 99], r9b        # 1-byte Spill
	mov	rax, qword ptr [rcx]
	mov	r8, qword ptr [rcx + 8]
	mov	qword ptr [rsp + 80], rcx       # 8-byte Spill
	lea	rsi, [rcx + 22856]
	mov	rcx, rsi
	mov	r9, rdx
	mov	rdx, rax
	mov	qword ptr [rsp + 200], r9       # 8-byte Spill
	call	"?d3d11_draw_pipeline_update_background@maicord@@YA_NPEAUD3D11DrawPipelineOwner@1@PEAUID3D11Device@@PEAUID3D11DeviceContext@@AEBUTheme@1@@Z"
	test	al, al
	je	.LBB11_5
# %bb.12:
	mov	r9, qword ptr [rsp + 80]        # 8-byte Reload
	mov	rax, qword ptr [r9 + 88]
	test	rax, rax
	mov	rcx, qword ptr [rsp + 200]      # 8-byte Reload
	mov	r8d, ebx
	jne	.LBB11_21
# %bb.13:
	cmp	byte ptr [r9 + 120537], 0
	jne	.LBB11_20
# %bb.17:
	cmp	byte ptr [r9 + 23628], 1
	jne	.LBB11_20
# %bb.18:
	cmp	dword ptr [r9 + 23616], 0
	je	.LBB11_156
.LBB11_20:
	xor	eax, eax
.LBB11_21:
	lea	rdx, [rax + 132]
	lea	r10, [r9 + 23616]
	test	rax, rax
	cmove	rdx, r10
	mov	ebp, dword ptr [rdx]
	mov	eax, dword ptr [r9 + 82728]
	or	eax, ebp
	jne	.LBB11_23
# %bb.22:
	cmp	dword ptr [r9 + 96], 0
	je	.LBB11_37
.LBB11_23:
	mov	ebx, r8d
	call	"?ui_icon_theme_weight@maicord@@YAMAEBUTheme@1@@Z"
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	test	bl, bl
	jne	.LBB11_25
# %bb.24:
	mov	edx, dword ptr [r8 + 112]
	lea	rcx, [r8 + 23632]
	vmovaps	xmm2, xmm0
	vmovaps	xmm6, xmm0
	call	"?icon_atlas_stale@maicord@@YA_NAEBUIconAtlas@1@IM@Z"
	vmovaps	xmm0, xmm6
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	mov	ebx, dword ptr [r8 + 82728]
	mov	ecx, dword ptr [r8 + 99232]
	cmp	ecx, ebx
	cmova	ebx, ecx
	test	al, al
	je	.LBB11_29
.LBB11_25:
	vxorps	xmm1, xmm1, xmm1
	vmovaps	xmmword ptr [rsp + 448], xmm1
.LBB11_26:
	mov	r9d, dword ptr [r8 + 82728]
	test	r9d, r9d
	je	.LBB11_47
# %bb.27:
	vmovaps	xmm6, xmm0
	mov	rdx, qword ptr [r8]
	mov	rcx, rsi
	mov	r8d, 64
	call	"?d3d11_draw_pipeline_reserve_ui_instances@maicord@@YA?AUD3D11DrawPipelineResult@1@PEAUD3D11DrawPipelineOwner@1@PEAUID3D11Device@@II@Z"
	mov	rsi, rax
	shr	rax, 32
	test	al, al
	je	.LBB11_38
# %bb.28:
	lea	rdi, [rip + "??_C@_06IAAOMEKN@render?$AA@"]
	lea	r8, [rip + "??_C@_0CB@HLLCFHNL@ui_instance_buffer_growth_failed@"]
	mov	cl, 3
	mov	rdx, rdi
	call	"?log_write@maicord@@YAXW4LogLevel@1@PEBD1@Z"
	mov	r9d, esi
	lea	r8, [rip + "??_C@_0BJ@MLLFCGOM@ui_instance_native_error?$AA@"]
	mov	cl, 3
	mov	rdx, rdi
	call	"?log_u64@maicord@@YAXW4LogLevel@1@PEBD1_K@Z"
.LBB11_5:
	xor	eax, eax
.LBB11_6:
                                        # kill: def $al killed $al killed $eax
	vmovaps	xmm6, xmmword ptr [rsp + 1472]  # 16-byte Reload
	vmovaps	xmm7, xmmword ptr [rsp + 1488]  # 16-byte Reload
	vmovaps	xmm8, xmmword ptr [rsp + 1504]  # 16-byte Reload
	vmovaps	xmm9, xmmword ptr [rsp + 1520]  # 16-byte Reload
	vmovaps	xmm10, xmmword ptr [rsp + 1536] # 16-byte Reload
	vmovaps	xmm11, xmmword ptr [rsp + 1552] # 16-byte Reload
	vmovaps	xmm12, xmmword ptr [rsp + 1568] # 16-byte Reload
	vmovaps	xmm13, xmmword ptr [rsp + 1584] # 16-byte Reload
	.seh_startepilogue
	add	rsp, 1608
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
.LBB11_29:
	cmp	byte ptr [r8 + 99244], 1
	jne	.LBB11_43
# %bb.30:
	mov	eax, dword ptr [r8 + 22992]
	sub	eax, ebx
	jb	.LBB11_25
# %bb.31:
	cmp	eax, dword ptr [r8 + 99220]
	vxorps	xmm1, xmm1, xmm1
	vmovaps	xmmword ptr [rsp + 448], xmm1
	jb	.LBB11_26
# %bb.32:
	mov	rdx, qword ptr [r8 + 22880]
	test	rdx, rdx
	je	.LBB11_44
# %bb.33:
	mov	rcx, qword ptr [r8 + 8]
	mov	rax, qword ptr [rcx]
	lea	r8, [rsp + 448]
	mov	qword ptr [rsp + 40], r8
	mov	dword ptr [rsp + 32], 0
	xor	r8d, r8d
	mov	r9d, 5
	call	qword ptr [rax + 112]
	mov	edi, eax
	test	eax, eax
	sets	al
	mov	rcx, qword ptr [rsp + 448]
	test	rcx, rcx
	setne	dl
	or	dl, al
	jne	.LBB11_35
# %bb.34:
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 8]
	mov	rdx, qword ptr [rax + 22880]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	mov	rcx, qword ptr [rsp + 448]
.LBB11_35:
	test	edi, edi
	setns	al
	test	rcx, rcx
	setne	dl
	and	dl, al
	cmp	dl, 1
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	vmovaps	xmm0, xmm6
	jne	.LBB11_25
# %bb.36:
	mov	edx, dword ptr [r8 + 99216]
	mov	esi, dword ptr [r8 + 99220]
	shl	rsi, 6
	mov	eax, ebx
	shl	rax, 6
	add	rcx, rax
	shl	rdx, 6
	add	rdx, qword ptr [r8 + 75520]
	mov	r8, rsi
	call	memcpy
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 8]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rdx, qword ptr [rax + 22880]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	mov	dword ptr [r8 + 99228], ebx
	add	ebx, dword ptr [r8 + 99220]
	mov	dword ptr [r8 + 99232], ebx
	mov	byte ptr [r8 + 99244], 0
	inc	dword ptr [r8 + 99240]
	add	qword ptr [r8 + 99352], rsi
	jmp	.LBB11_44
.LBB11_37:
	mov	al, 1
	jmp	.LBB11_6
.LBB11_38:
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 528], xmm0
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 8]
	mov	rdx, qword ptr [rax + 22880]
	mov	rax, qword ptr [rcx]
	lea	r8, [rsp + 528]
	mov	qword ptr [rsp + 40], r8
	mov	dword ptr [rsp + 32], 0
	xor	r8d, r8d
	mov	r9d, 4
	call	qword ptr [rax + 112]
	test	eax, eax
	js	.LBB11_151
# %bb.39:
	mov	r9, qword ptr [rsp + 528]
	test	r9, r9
	je	.LBB11_154
# %bb.40:
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r10d, dword ptr [rax + 112]
	mov	r11d, dword ptr [rax + 82728]
	mov	r8, qword ptr [rax + 75520]
	mov	rdx, qword ptr [rax + 8]
	lea	rcx, [rax + 23632]
	vmovss	dword ptr [rsp + 48], xmm6
	mov	dword ptr [rsp + 40], r10d
	mov	dword ptr [rsp + 32], r11d
	call	"?icon_atlas_resolve@maicord@@YA_NPEAUIconAtlas@1@PEAUID3D11DeviceContext@@PEBUPaintQuad@1@PEAU41@IIM@Z"
	test	al, al
	jne	.LBB11_42
# %bb.41:
	lea	rdx, [rip + "??_C@_06IAAOMEKN@render?$AA@"]
	lea	r8, [rip + "??_C@_0BJ@MGLHKPME@icon_atlas_raster_failed?$AA@"]
	mov	cl, 2
	call	"?log_write@maicord@@YAXW4LogLevel@1@PEBD1@Z"
.LBB11_42:
	mov	rsi, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rcx, qword ptr [rsi + 8]
	mov	rdx, qword ptr [rsi + 22880]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	mov	r8, rsi
	inc	dword ptr [rsi + 99348]
	mov	eax, dword ptr [rsi + 82728]
	mov	rcx, rax
	shl	rcx, 6
	add	qword ptr [rsi + 99352], rcx
	mov	ecx, dword ptr [rsi + 99216]
	mov	dword ptr [rsi + 99228], ecx
	mov	dword ptr [rsi + 99232], eax
	mov	byte ptr [rsi + 99244], 0
	jmp	.LBB11_44
.LBB11_43:
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 448], xmm0
.LBB11_44:
	mov	eax, dword ptr [r8 + 82728]
	test	eax, eax
	je	.LBB11_47
# %bb.45:
	cmp	qword ptr [r8 + 22880], 0
	je	.LBB11_5
# %bb.46:
	cmp	eax, dword ptr [r8 + 22992]
	ja	.LBB11_5
.LBB11_47:
	mov	rdx, qword ptr [r8 + 8]
	lea	rcx, [r8 + 69280]
	call	"?d3d11_gpu_timer_begin@maicord@@YAPEAUD3D11GpuTimerFrame@1@PEAUD3D11GpuTimerOwner@1@PEAUID3D11DeviceContext@@@Z"
	mov	qword ptr [rsp + 520], rax
	vxorps	xmm0, xmm0, xmm0
	vmovups	ymmword ptr [rsp + 1392], ymm0
	mov	qword ptr [rsp + 1424], 0
	vzeroupper
	call	qword ptr [rip + __imp_GetTickCount64]
	mov	rsi, rax
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r9d, dword ptr [rax + 112]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r10d, dword ptr [rax + 108]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r11d, dword ptr [rax + 104]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	vmovss	xmm0, dword ptr [rax + 144]     # xmm0 = mem[0],zero,zero,zero
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rdx, qword ptr [rax]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r8, qword ptr [rax + 8]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	lea	rcx, [rax + 69368]
	mov	qword ptr [rsp + 64], rsi
	mov	dword ptr [rsp + 56], r9d
	mov	dword ptr [rsp + 48], r10d
	mov	dword ptr [rsp + 40], r11d
	vmovss	dword ptr [rsp + 32], xmm0
	mov	r9, qword ptr [rsp + 200]       # 8-byte Reload
	call	"?d3d11_background_fx_begin_frame@maicord@@YA_NPEAUD3D11BackgroundFxOwner@1@PEAUID3D11Device@@PEAUID3D11DeviceContext@@AEBUTheme@1@MIII_K@Z"
	mov	qword ptr [rsp + 512], rsi      # 8-byte Spill
	lea	rcx, [rsi + 42]
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	qword ptr [rax + 20680], rcx
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_caption_begin_scrim@maicord@@YAXPEAUD3D11Renderer@1@@Z"
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	dword ptr [rax + 99312], 0
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	dword ptr [rax + 99320], 0
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	eax, dword ptr [rax + 112]
	vcvtsi2ss	xmm0, xmm5, rax
	vdivss	xmm0, xmm0, dword ptr [rip + __real@42c00000]
	vmovss	dword ptr [rsp + 260], xmm0
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	byte ptr [rsp + 79], al
	mov	rcx, qword ptr [rdx + 64000]
	test	rcx, rcx
	je	.LBB11_51
# %bb.48:
	cmp	byte ptr [rdx + 120537], 0
	jne	.LBB11_51
# %bb.49:
	call	"?ui_toast_visible@maicord@@YA_NAEBUUiToastScene@1@@Z"
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	ecx, dword ptr [rdx + 82736]
	test	al, al
	je	.LBB11_52
# %bb.50:
	mov	eax, dword ptr [rdx + 64008]
	cmp	eax, ecx
	cmovae	eax, ecx
	mov	dil, 1
	jmp	.LBB11_53
.LBB11_51:
	mov	ecx, dword ptr [rdx + 82736]
.LBB11_52:
	xor	edi, edi
	mov	eax, ecx
.LBB11_53:
	mov	dword ptr [rsp + 92], eax
	mov	rax, qword ptr [rdx + 88]
	test	rax, rax
	je	.LBB11_55
# %bb.54:
	mov	esi, dword ptr [rax + 136]
	jmp	.LBB11_56
.LBB11_55:
	mov	esi, 1
.LBB11_56:
	cmp	byte ptr [rdx + 120537], 1
	mov	dword ptr [rsp + 360], ecx      # 4-byte Spill
	jne	.LBB11_62
# %bb.57:
	cmp	qword ptr [rdx + 68792], 0
	je	.LBB11_61
# %bb.58:
	mov	rax, qword ptr [rdx + 63880]
	test	rax, rax
	je	.LBB11_61
# %bb.59:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB11_61
# %bb.60:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB11_101
.LBB11_61:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_interaction_text_state_current@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	byte ptr [rsp + 91], al
	lea	rdx, [rsp + 92]
	xor	r10d, r10d
	mov	r9, qword ptr [rcx + 88]
	mov	r8d, esi
	test	r9, r9
	jne	.LBB11_102
	jmp	.LBB11_103
.LBB11_62:
	mov	rcx, qword ptr [rdx + 63976]
	test	rcx, rcx
	je	.LBB11_64
# %bb.63:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
                                        # kill: def $al killed $al def $eax
	jmp	.LBB11_65
.LBB11_64:
	xor	eax, eax
.LBB11_65:
	test	al, al
	jne	.LBB11_101
# %bb.66:
	cmp	byte ptr [rsp + 79], 0
	jne	.LBB11_101
# %bb.67:
	mov	rax, qword ptr [rdx + 22736]
	test	rax, rax
	je	.LBB11_69
# %bb.68:
	cmp	byte ptr [rax + 8583], 0
	jne	.LBB11_101
.LBB11_69:
	mov	rax, qword ptr [rdx + 64032]
	test	rax, rax
	je	.LBB11_71
# %bb.70:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB11_101
.LBB11_71:
	mov	rax, qword ptr [rdx + 63904]
	test	rax, rax
	je	.LBB11_73
# %bb.72:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB11_101
.LBB11_73:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.74:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.75:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.76:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.77:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.78:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.79:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	or	al, dil
	jne	.LBB11_101
# %bb.80:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.81:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.82:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.83:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.84:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.85:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.86:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.87:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.88:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.89:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_101
# %bb.90:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	jne	.LBB11_101
# %bb.91:
	mov	rax, qword ptr [rcx + 61080]
	test	rax, rax
	je	.LBB11_93
# %bb.92:
	cmp	byte ptr [rax + 572], 0
	jne	.LBB11_101
.LBB11_93:
	mov	rax, qword ptr [rcx + 61104]
	test	rax, rax
	je	.LBB11_95
# %bb.94:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB11_101
.LBB11_95:
	mov	rax, qword ptr [rcx + 63880]
	test	rax, rax
	je	.LBB11_98
# %bb.96:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB11_98
# %bb.97:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB11_101
.LBB11_98:
	mov	rax, qword ptr [rcx + 63888]
	test	rax, rax
	je	.LBB11_292
# %bb.99:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB11_292
# %bb.100:
	cmp	byte ptr [rax + 1428], 0
	je	.LBB11_292
.LBB11_101:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_interaction_text_state_current@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	byte ptr [rsp + 91], al
	lea	rdx, [rcx + 99472]
	mov	r10d, eax
	mov	r9, qword ptr [rcx + 88]
	mov	r8d, esi
	test	r9, r9
	je	.LBB11_103
.LBB11_102:
	mov	r8d, dword ptr [rcx + 99480]
	cmp	r8d, esi
	cmovae	r8d, esi
.LBB11_103:
	test	r10b, r10b
	mov	r11d, r8d
	cmovne	r11d, esi
	mov	r10d, r11d
	test	dil, dil
	je	.LBB11_105
# %bb.104:
	mov	r10d, dword ptr [rcx + 64012]
	cmp	r10d, r11d
	cmovae	r10d, r11d
	cmp	r8d, r10d
	cmova	r10d, r8d
.LBB11_105:
	mov	edx, dword ptr [rdx]
	mov	dword ptr [rsp + 136], r10d
	cmp	esi, r8d
	setbe	r10b
	or	al, r10b
	jne	.LBB11_108
# %bb.106:
	mov	eax, dword ptr [rcx + 120384]
	inc	eax
	je	.LBB11_108
# %bb.107:
	mov	dword ptr [rcx + 120384], eax
.LBB11_108:
	mov	dword ptr [rsp + 364], edi      # 4-byte Spill
	mov	rax, qword ptr [rcx + 22736]
	test	rax, rax
	je	.LBB11_111
# %bb.109:
	mov	edi, dword ptr [rsp + 92]
	cmp	byte ptr [rax + 8583], 1
	jne	.LBB11_112
# %bb.110:
	mov	r10d, dword ptr [rcx + 99420]
	mov	eax, dword ptr [rcx + 99424]
	cmp	r10d, edi
	cmovae	r10d, edi
	cmp	eax, edi
	cmovb	edi, eax
	cmp	r10d, edi
	mov	eax, edi
	cmova	eax, r10d
	mov	edi, r10d
	jmp	.LBB11_113
.LBB11_111:
	mov	edi, dword ptr [rsp + 92]
.LBB11_112:
	mov	eax, edi
.LBB11_113:
	mov	dword ptr [rsp + 356], r11d     # 4-byte Spill
	test	r9, r9
	je	.LBB11_115
# %bb.114:
	mov	r9d, dword ptr [rcx + 99416]
	cmp	r9d, edx
	jbe	.LBB11_116
.LBB11_115:
	mov	r9d, edx
.LBB11_116:
	mov	dword ptr [rsp + 180], r9d      # 4-byte Spill
	mov	r11d, dword ptr [rcx + 68820]
	mov	r9d, dword ptr [rcx + 100]
	cmp	r11d, r9d
	jne	.LBB11_118
# %bb.117:
	mov	r9d, dword ptr [rcx + 68816]
	cmp	r9d, r11d
	setbe	r10b
	cmovb	r11d, r9d
	mov	r9d, r11d
	jmp	.LBB11_119
.LBB11_118:
	xor	r10d, r10d
.LBB11_119:
	mov	dword ptr [rsp + 372], ebp      # 4-byte Spill
	mov	dword ptr [rsp + 196], r9d      # 4-byte Spill
	mov	byte ptr [rsp + 147], r10b
	mov	r11d, dword ptr [rcx + 99456]
	cmp	r11d, r9d
	cmovae	r11d, r9d
	mov	dword ptr [rsp + 188], r11d     # 4-byte Spill
	mov	r10d, dword ptr [rcx + 99524]
	cmp	r10d, r11d
	cmovae	r10d, r11d
	mov	r9d, dword ptr [rcx + 99528]
	cmp	r9d, r11d
	cmovae	r9d, r11d
	cmp	r10d, r9d
	cmova	r9d, r10d
	mov	dword ptr [rsp + 112], r9d      # 4-byte Spill
	xor	r9d, r9d
	cmp	byte ptr [rcx + 120537], 0
	mov	dword ptr [rsp + 368], eax      # 4-byte Spill
	mov	dword ptr [rsp + 156], r8d      # 4-byte Spill
	mov	dword ptr [rsp + 104], r10d     # 4-byte Spill
	mov	dword ptr [rsp + 148], edx      # 4-byte Spill
	jne	.LBB11_128
# %bb.120:
	cmp	byte ptr [rsp + 79], 0
	mov	r14d, r10d
	mov	r11d, edx
	mov	ebp, edx
	mov	r15d, r10d
	jne	.LBB11_129
# %bb.121:
	mov	rax, qword ptr [rcx + 64032]
	test	rax, rax
	je	.LBB11_123
# %bb.122:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB11_126
.LBB11_123:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	jne	.LBB11_126
# %bb.124:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	jne	.LBB11_126
# %bb.125:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	je	.LBB11_163
.LBB11_126:
	mov	ebp, dword ptr [rsp + 92]
	mov	r11d, dword ptr [rcx + 99484]
	mov	eax, dword ptr [rcx + 99488]
	cmp	r11d, ebp
	cmovae	r11d, ebp
	mov	edx, dword ptr [rsp + 148]      # 4-byte Reload
	cmp	edx, r11d
	cmova	r11d, edx
	cmp	eax, ebp
	cmovb	ebp, eax
	cmp	r11d, ebp
	cmova	ebp, r11d
	mov	r14d, dword ptr [rcx + 99492]
	mov	eax, dword ptr [rsp + 104]      # 4-byte Reload
	cmp	r14d, eax
	cmovae	r14d, eax
	mov	r15d, dword ptr [rcx + 99496]
	cmp	r15d, eax
	cmovae	r15d, eax
	cmp	r14d, r15d
	cmova	r15d, r14d
	mov	r9b, 1
.LBB11_127:
	mov	r8d, dword ptr [rsp + 156]      # 4-byte Reload
	jmp	.LBB11_129
.LBB11_128:
	mov	r14d, r10d
	mov	r11d, edx
	mov	ebp, edx
	mov	r15d, r10d
.LBB11_129:
	mov	dword ptr [rsp + 236], r11d     # 4-byte Spill
	mov	r12d, dword ptr [rcx + 99500]
	cmp	r12d, r14d
	cmovae	r12d, r14d
	mov	r13d, dword ptr [rcx + 99504]
	cmp	r13d, r14d
	cmovae	r13d, r14d
	cmp	r12d, r13d
	cmova	r13d, r12d
	mov	esi, r8d
	test	r9b, r9b
	je	.LBB11_131
# %bb.130:
	mov	esi, dword ptr [rsp + 136]
	mov	eax, dword ptr [rcx + 99508]
	cmp	eax, esi
	cmovb	esi, eax
	cmp	r8d, esi
	cmova	esi, r8d
.LBB11_131:
	cmp	byte ptr [rcx + 120537], 0
	mov	ebx, edx
	jne	.LBB11_134
# %bb.132:
	cmp	qword ptr [rcx + 88], 0
	mov	ebx, edx
	je	.LBB11_134
# %bb.133:
	mov	ebx, dword ptr [rcx + 99476]
	cmp	edx, ebx
	cmovb	ebx, edx
	mov	eax, dword ptr [rsp + 180]      # 4-byte Reload
	cmp	eax, ebx
	cmova	ebx, eax
.LBB11_134:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	mov	r8d, dword ptr [rsp + 196]      # 4-byte Reload
	mov	edx, dword ptr [rsp + 188]      # 4-byte Reload
	mov	eax, dword ptr [rsp + 156]      # 4-byte Reload
	mov	dword ptr [rsp + 232], r15d     # 4-byte Spill
	mov	dword ptr [rsp + 244], r14d     # 4-byte Spill
	mov	dword ptr [rsp + 240], ebx      # 4-byte Spill
	mov	dword ptr [rsp + 248], r13d     # 4-byte Spill
	mov	dword ptr [rsp + 252], r12d     # 4-byte Spill
	je	.LBB11_136
.LBB11_135:
	mov	r10d, dword ptr [rsp + 92]
	mov	r11d, dword ptr [rcx + 120340]
	mov	r9d, dword ptr [rcx + 120344]
	cmp	r9d, r10d
	cmovb	r10d, r9d
	mov	r9d, dword ptr [rsp + 136]
	cmp	r11d, r9d
	cmovb	r9d, r11d
	jmp	.LBB11_145
.LBB11_136:
	mov	r9, qword ptr [rcx + 63880]
	test	r9, r9
	je	.LBB11_139
# %bb.137:
	mov	r9, qword ptr [r9]
	test	r9, r9
	je	.LBB11_139
# %bb.138:
	cmp	byte ptr [r9 + 704], 0
	jne	.LBB11_135
.LBB11_139:
	mov	r9, qword ptr [rcx + 63888]
	test	r9, r9
	je	.LBB11_143
# %bb.140:
	mov	r9, qword ptr [r9]
	test	r9, r9
	je	.LBB11_143
# %bb.141:
	cmp	byte ptr [r9 + 1428], 0
	jne	.LBB11_135
# %bb.142:
	cmp	byte ptr [rsp + 79], 0
	je	.LBB11_144
	jmp	.LBB11_135
.LBB11_143:
	cmp	byte ptr [rsp + 79], 1
	je	.LBB11_135
.LBB11_144:
	mov	r10d, dword ptr [rsp + 92]
	mov	r9d, dword ptr [rsp + 136]
.LBB11_145:
	mov	r11d, dword ptr [rsp + 148]     # 4-byte Reload
	mov	ebx, dword ptr [rcx + 99460]
	cmp	ebx, r10d
	mov	dword ptr [rsp + 128], r10d     # 4-byte Spill
	cmovae	ebx, r10d
	cmp	r11d, ebx
	cmova	ebx, r11d
	mov	r10d, dword ptr [rcx + 99464]
	cmp	r10d, r8d
	cmovae	r10d, r8d
	cmp	edx, r10d
	cmova	r10d, edx
	mov	r8d, dword ptr [rcx + 99448]
	cmp	r8d, r10d
	mov	dword ptr [rsp + 124], r10d     # 4-byte Spill
	cmovae	r8d, r10d
	cmp	edx, r8d
	cmova	r8d, edx
	mov	dword ptr [rsp + 212], r8d      # 4-byte Spill
	mov	edx, dword ptr [rcx + 99468]
	cmp	edx, r9d
	mov	dword ptr [rsp + 108], r9d      # 4-byte Spill
	cmovae	edx, r9d
	cmp	eax, edx
	cmova	edx, eax
	cmp	edi, ebx
	mov	dword ptr [rsp + 140], ebx      # 4-byte Spill
	cmovae	edi, ebx
	mov	r14d, dword ptr [rcx + 99512]
	cmp	r14d, edi
	cmovae	r14d, edi
	cmp	ebp, r14d
	mov	dword ptr [rsp + 228], ebp      # 4-byte Spill
	cmova	r14d, ebp
	mov	ebp, dword ptr [rcx + 99516]
	cmp	ebp, edi
	mov	dword ptr [rsp + 216], edi      # 4-byte Spill
	cmovae	ebp, edi
	cmp	r14d, ebp
	cmova	ebp, r14d
	mov	eax, dword ptr [rcx + 99532]
	cmp	eax, edx
	cmovae	eax, edx
	cmp	esi, eax
	mov	dword ptr [rsp + 224], esi      # 4-byte Spill
	cmova	eax, esi
	mov	r8d, dword ptr [rcx + 99536]
	cmp	r8d, edx
	mov	dword ptr [rsp + 152], edx      # 4-byte Spill
	cmovae	r8d, edx
	cmp	eax, r8d
	mov	dword ptr [rsp + 192], eax      # 4-byte Spill
	cmova	r8d, eax
	mov	dword ptr [rsp + 184], r8d      # 4-byte Spill
	mov	rbx, rcx
	call	"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	byte ptr [rsp + 89], al         # 1-byte Spill
	mov	eax, dword ptr [rbx + 99520]
	cmp	eax, ebp
	mov	dword ptr [rsp + 116], ebp      # 4-byte Spill
	cmovae	eax, ebp
	cmp	r14d, eax
	mov	dword ptr [rsp + 132], r14d     # 4-byte Spill
	cmova	eax, r14d
	mov	dword ptr [rsp + 220], eax      # 4-byte Spill
	mov	rcx, rbx
	call	"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	sil, 1
	test	al, al
	jne	.LBB11_146
# %bb.147:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_146
# %bb.149:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB11_146
# %bb.152:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB11_158
.LBB11_146:
	movzx	eax, byte ptr [rsp + 89]        # 1-byte Folded Reload
.LBB11_167:
	movzx	edx, byte ptr [rsp + 79]
	cmp	dl, 1
	jne	.LBB11_171
# %bb.168:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r8, qword ptr [rcx + 61104]
	test	r8, r8
	je	.LBB11_173
# %bb.169:
	cmp	byte ptr [r8 + 398], 1
	jne	.LBB11_173
# %bb.170:
	cmp	byte ptr [r8 + 396], 5
	sete	r8b
	jmp	.LBB11_174
.LBB11_171:
	xor	r8d, r8d
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	jmp	.LBB11_174
.LBB11_173:
	xor	r8d, r8d
.LBB11_174:
	vmovss	xmm6, dword ptr [rip + __real@3f800000] # xmm6 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vmovaps	xmm0, xmm6
	vmovaps	xmm1, xmm6
	test	sil, sil
	je	.LBB11_176
# %bb.175:
	vmovss	xmm0, dword ptr [rcx + 120]     # xmm0 = mem[0],zero,zero,zero
	vmovss	xmm1, dword ptr [rcx + 120328]  # xmm1 = mem[0],zero,zero,zero
.LBB11_176:
	mov	r9, qword ptr [rcx + 22736]
	vmovaps	xmm2, xmm6
	test	r9, r9
	je	.LBB11_180
# %bb.177:
	cmp	byte ptr [r9 + 8583], 1
	vmovaps	xmm2, xmm6
	jne	.LBB11_180
# %bb.178:
	movzx	r9d, byte ptr [rcx + 120537]
	or	r9b, dl
	vmovaps	xmm2, xmm6
	jne	.LBB11_180
# %bb.179:
	vmovss	xmm2, dword ptr [rcx + 136]     # xmm2 = mem[0],zero,zero,zero
.LBB11_180:
	vmovaps	xmm3, xmm6
	test	r8b, r8b
	je	.LBB11_182
# %bb.181:
	vmovss	xmm3, dword ptr [rcx + 124]     # xmm3 = mem[0],zero,zero,zero
.LBB11_182:
	vmovaps	xmm4, xmm6
	test	al, al
	je	.LBB11_184
# %bb.183:
	vmovss	xmm4, dword ptr [rcx + 116]     # xmm4 = mem[0],zero,zero,zero
.LBB11_184:
	vmovss	dword ptr [rsp + 172], xmm4     # 4-byte Spill
	vmovss	dword ptr [rsp + 100], xmm2     # 4-byte Spill
	vmovss	dword ptr [rsp + 120], xmm3     # 4-byte Spill
	vmovss	dword ptr [rsp + 164], xmm1     # 4-byte Spill
	vmovss	dword ptr [rsp + 168], xmm0     # 4-byte Spill
	vmovaps	xmm0, xmm6
	test	dl, dl
	je	.LBB11_186
# %bb.185:
	vmovss	xmm0, dword ptr [rcx + 128]     # xmm0 = mem[0],zero,zero,zero
.LBB11_186:
	vmovss	dword ptr [rsp + 160], xmm0     # 4-byte Spill
	vmovsd	xmm0, qword ptr [rcx + 120332]  # xmm0 = mem[0],zero
	vmovaps	xmmword ptr [rsp + 432], xmm0   # 16-byte Spill
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	mov	ebx, eax
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	or	al, bl
	vmovaps	xmm1, xmm6
	cmp	al, 1
	jne	.LBB11_188
# %bb.187:
	vmovss	xmm1, dword ptr [r8 + 99736]    # xmm1 = mem[0],zero,zero,zero
.LBB11_188:
	vxorps	xmm0, xmm0, xmm0
	vcmpless	xmm2, xmm0, xmm1
	vminss	xmm1, xmm1, xmm6
	vbroadcastss	xmm3, dword ptr [rip + __real@3f800000] # xmm3 = [1.0E+0,1.0E+0,1.0E+0,1.0E+0]
	vblendvps	xmm7, xmm3, xmm1, xmm2
	vucomiss	xmm7, xmm6
	setae	cl
	or	cl, bl
	mov	esi, dword ptr [rsp + 128]      # 4-byte Reload
	mov	r12d, dword ptr [rsp + 108]     # 4-byte Reload
	mov	r13d, dword ptr [rsp + 112]     # 4-byte Reload
	mov	r15d, dword ptr [rsp + 104]     # 4-byte Reload
	mov	ebp, dword ptr [rsp + 152]      # 4-byte Reload
	mov	edi, dword ptr [rsp + 124]      # 4-byte Reload
	mov	r14d, dword ptr [rsp + 140]     # 4-byte Reload
	mov	r9d, dword ptr [rsp + 116]      # 4-byte Reload
	mov	edx, dword ptr [rsp + 132]      # 4-byte Reload
	je	.LBB11_190
# %bb.189:
	vminss	xmm8, xmm6, xmm7
	jmp	.LBB11_191
.LBB11_190:
	vaddss	xmm1, xmm7, dword ptr [rip + __real@bdf5c28f]
	vdivss	xmm1, xmm1, dword ptr [rip + __real@3f6147ae]
	vmaxss	xmm8, xmm0, xmm1
.LBB11_191:
	vmovss	xmm2, dword ptr [rsp + 168]     # 4-byte Reload
                                        # xmm2 = mem[0],zero,zero,zero
	vmovss	xmm3, dword ptr [rsp + 164]     # 4-byte Reload
                                        # xmm3 = mem[0],zero,zero,zero
	vmovss	xmm1, dword ptr [rsp + 120]     # 4-byte Reload
                                        # xmm1 = mem[0],zero,zero,zero
	vmovss	xmm0, dword ptr [rsp + 160]     # 4-byte Reload
                                        # xmm0 = mem[0],zero,zero,zero
	vmovss	xmm10, dword ptr [rsp + 100]    # 4-byte Reload
                                        # xmm10 = mem[0],zero,zero,zero
	vmovss	xmm5, dword ptr [rsp + 172]     # 4-byte Reload
                                        # xmm5 = mem[0],zero,zero,zero
	vmovaps	xmm4, xmmword ptr [rsp + 432]   # 16-byte Reload
	vmovss	xmm6, dword ptr [rip + __real@3f800000] # xmm6 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vucomiss	xmm7, xmm6
	jae	.LBB11_195
# %bb.192:
	test	bl, bl
	je	.LBB11_194
# %bb.193:
	vmulss	xmm6, xmm7, dword ptr [rip + __real@bdcccccd]
	vaddss	xmm6, xmm6, dword ptr [rip + __real@3f8ccccd]
	jmp	.LBB11_196
.LBB11_194:
	vmulss	xmm6, xmm7, dword ptr [rip + __real@3dcccccd]
	vaddss	xmm6, xmm6, dword ptr [rip + __real@3f666666]
.LBB11_195:
	test	al, al
	je	.LBB11_197
.LBB11_196:
	mov	rax, qword ptr [r8 + 63968]
	vmovsd	xmm9, qword ptr [rax + 760]     # xmm9 = mem[0],zero
	vmovsd	xmm11, qword ptr [rax + 768]    # xmm11 = mem[0],zero
	vaddps	xmm9, xmm9, xmm11
	vmulps	xmm11, xmm9, xmmword ptr [rip + __xmm@00000000000000003f0000003f000000]
	vmovsd	xmm9, qword ptr [r8 + 99728]    # xmm9 = mem[0],zero
	mov	ebx, dword ptr [r8 + 99588]
	jmp	.LBB11_198
.LBB11_197:
	vxorps	xmm11, xmm11, xmm11
	vxorps	xmm9, xmm9, xmm9
	xor	ebx, ebx
.LBB11_198:
	cmp	byte ptr [r8 + 120537], 0
	vmovaps	xmmword ptr [rsp + 1456], xmm11 # 16-byte Spill
	vmovss	dword ptr [rsp + 380], xmm6     # 4-byte Spill
	vmovaps	xmmword ptr [rsp + 1440], xmm7  # 16-byte Spill
	mov	dword ptr [rsp + 376], ebx      # 4-byte Spill
	je	.LBB11_200
# %bb.199:
	mov	byte ptr [rsp + 90], 0
	vmovss	xmm6, dword ptr [rip + __real@3f800000] # xmm6 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vmovaps	xmm7, xmm6
	jmp	.LBB11_211
.LBB11_200:
	mov	rax, qword ptr [r8 + 63888]
	test	rax, rax
	je	.LBB11_204
# %bb.201:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB11_204
# %bb.202:
	movzx	eax, byte ptr [rax + 1428]
	mov	byte ptr [rsp + 90], al
	cmp	al, 1
	jne	.LBB11_205
# %bb.203:
	vmovss	xmm6, dword ptr [r8 + 140]      # xmm6 = mem[0],zero,zero,zero
	jmp	.LBB11_206
.LBB11_204:
	mov	byte ptr [rsp + 90], 0
.LBB11_205:
	vmovss	xmm6, dword ptr [rip + __real@3f800000] # xmm6 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
.LBB11_206:
	mov	rcx, qword ptr [r8 + 63976]
	vmovss	xmm7, dword ptr [rip + __real@3f800000] # xmm7 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	test	rcx, rcx
	je	.LBB11_211
# %bb.207:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB11_209
# %bb.208:
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	vmovss	xmm7, dword ptr [r8 + 132]      # xmm7 = mem[0],zero,zero,zero
	jmp	.LBB11_210
.LBB11_151:
	lea	rdx, [rip + "??_C@_06IAAOMEKN@render?$AA@"]
	lea	r8, [rip + "??_C@_0BO@DKOJHADH@ui_instance_buffer_map_failed?$AA@"]
	mov	cl, 3
	call	"?log_write@maicord@@YAXW4LogLevel@1@PEBD1@Z"
	jmp	.LBB11_5
.LBB11_209:
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
.LBB11_210:
	mov	esi, dword ptr [rsp + 128]      # 4-byte Reload
	mov	r12d, dword ptr [rsp + 108]     # 4-byte Reload
	mov	r13d, dword ptr [rsp + 112]     # 4-byte Reload
	mov	r15d, dword ptr [rsp + 104]     # 4-byte Reload
	mov	ebp, dword ptr [rsp + 152]      # 4-byte Reload
	mov	edi, dword ptr [rsp + 124]      # 4-byte Reload
	mov	r14d, dword ptr [rsp + 140]     # 4-byte Reload
	mov	r9d, dword ptr [rsp + 116]      # 4-byte Reload
	mov	edx, dword ptr [rsp + 132]      # 4-byte Reload
	vmovss	xmm2, dword ptr [rsp + 168]     # 4-byte Reload
                                        # xmm2 = mem[0],zero,zero,zero
	vmovss	xmm3, dword ptr [rsp + 164]     # 4-byte Reload
                                        # xmm3 = mem[0],zero,zero,zero
	vmovss	xmm1, dword ptr [rsp + 120]     # 4-byte Reload
                                        # xmm1 = mem[0],zero,zero,zero
	vmovss	xmm0, dword ptr [rsp + 160]     # 4-byte Reload
                                        # xmm0 = mem[0],zero,zero,zero
	vmovss	xmm10, dword ptr [rsp + 100]    # 4-byte Reload
                                        # xmm10 = mem[0],zero,zero,zero
	vmovss	xmm5, dword ptr [rsp + 172]     # 4-byte Reload
                                        # xmm5 = mem[0],zero,zero,zero
	vmovaps	xmm4, xmmword ptr [rsp + 432]   # 16-byte Reload
.LBB11_211:
	mov	r10d, dword ptr [r8 + 99596]
	cmp	r10d, esi
	cmovae	r10d, esi
	cmp	r14d, r10d
	cmova	r10d, r14d
	mov	dword ptr [rsp + 176], r10d     # 4-byte Spill
	mov	ebx, dword ptr [r8 + 99600]
	cmp	ebx, esi
	cmovae	ebx, esi
	cmp	r10d, ebx
	cmova	ebx, r10d
	mov	r14d, dword ptr [r8 + 99612]
	cmp	r14d, r12d
	cmovae	r14d, r12d
	cmp	ebp, r14d
	cmova	r14d, ebp
	mov	ebp, dword ptr [r8 + 99616]
	cmp	ebp, r12d
	cmovae	ebp, r12d
	cmp	r14d, ebp
	cmova	ebp, r14d
	cmp	byte ptr [rsp + 79], 0
	jne	.LBB11_216
# %bb.212:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	vmovaps	xmm4, xmmword ptr [rsp + 432]   # 16-byte Reload
	vmovss	xmm5, dword ptr [rsp + 172]     # 4-byte Reload
                                        # xmm5 = mem[0],zero,zero,zero
	vmovss	xmm10, dword ptr [rsp + 100]    # 4-byte Reload
                                        # xmm10 = mem[0],zero,zero,zero
	vmovss	xmm0, dword ptr [rsp + 160]     # 4-byte Reload
                                        # xmm0 = mem[0],zero,zero,zero
	vmovss	xmm1, dword ptr [rsp + 120]     # 4-byte Reload
                                        # xmm1 = mem[0],zero,zero,zero
	vmovss	xmm3, dword ptr [rsp + 164]     # 4-byte Reload
                                        # xmm3 = mem[0],zero,zero,zero
	vmovss	xmm2, dword ptr [rsp + 168]     # 4-byte Reload
                                        # xmm2 = mem[0],zero,zero,zero
	mov	r11d, dword ptr [rsp + 176]     # 4-byte Reload
	mov	edx, dword ptr [rsp + 132]      # 4-byte Reload
	mov	r9d, dword ptr [rsp + 116]      # 4-byte Reload
	mov	ecx, dword ptr [rsp + 124]      # 4-byte Reload
	mov	r15d, dword ptr [rsp + 104]     # 4-byte Reload
	mov	r13d, dword ptr [rsp + 112]     # 4-byte Reload
	mov	r12d, dword ptr [rsp + 108]     # 4-byte Reload
	mov	esi, dword ptr [rsp + 128]      # 4-byte Reload
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	mov	r10d, r11d
	mov	edi, ecx
	test	al, al
	je	.LBB11_216
# %bb.213:
	mov	eax, dword ptr [r8 + 99564]
	mov	r10d, r11d
	mov	edi, ecx
	cmp	eax, dword ptr [rsp + 140]      # 4-byte Folded Reload
	jb	.LBB11_216
# %bb.214:
	mov	r10d, r11d
	mov	edi, ecx
	cmp	eax, r11d
	ja	.LBB11_216
# %bb.215:
	mov	edi, dword ptr [r8 + 99568]
	cmp	edi, ecx
	setae	byte ptr [rsp + 146]            # 1-byte Folded Spill
	cmp	edi, dword ptr [rsp + 196]      # 4-byte Folded Reload
	mov	r10d, ebp
	mov	ebp, ebx
	setbe	bl
	test	byte ptr [rsp + 146], bl        # 1-byte Folded Reload
	mov	ebx, ebp
	mov	ebp, r10d
	cmove	eax, r11d
	cmove	edi, ecx
	mov	r10d, eax
.LBB11_216:
	mov	dword ptr [rsp + 120], r10d     # 4-byte Spill
	mov	dword ptr [rsp + 100], edi      # 4-byte Spill
	mov	dword ptr [rsp + 1064], 0
	vmovss	dword ptr [rsp + 1068], xmm9
	vmovaps	xmm11, xmmword ptr [rsp + 1440] # 16-byte Reload
	vmovss	dword ptr [rsp + 1072], xmm11
	vmovaps	xmm11, xmm9
	vmovss	xmm9, dword ptr [rip + __real@3f800000] # xmm9 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vmovlps	qword ptr [rsp + 1076], xmm9
	mov	dword ptr [rsp + 1084], 0
	vmovlps	qword ptr [rsp + 1088], xmm11
	vmovss	dword ptr [rsp + 1096], xmm8
	vmovss	xmm11, dword ptr [rsp + 380]    # 4-byte Reload
                                        # xmm11 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 1100], xmm11
	vmovaps	xmm12, xmmword ptr [rsp + 1456] # 16-byte Reload
	vmovss	dword ptr [rsp + 1104], xmm12
	vmovshdup	xmm13, xmm12            # xmm13 = xmm12[1,1,3,3]
	vmovss	dword ptr [rsp + 1108], xmm13
	mov	rax, qword ptr [r8 + 99428]
	mov	qword ptr [rsp + 1112], rax
	vmovss	dword ptr [rsp + 1120], xmm10
	vmovlps	qword ptr [rsp + 1124], xmm9
	mov	dword ptr [rsp + 1132], 0
	mov	dword ptr [rsp + 1136], edx
	mov	dword ptr [rsp + 1140], r9d
	vmovss	dword ptr [rsp + 1144], xmm5
	vmovlps	qword ptr [rsp + 1148], xmm9
	mov	dword ptr [rsp + 1156], 0
	mov	rax, qword ptr [r8 + 99540]
	mov	qword ptr [rsp + 1160], rax
	vmovss	dword ptr [rsp + 1168], xmm2
	vmovss	dword ptr [rsp + 1172], xmm3
	vmovlps	qword ptr [rsp + 1176], xmm4
	mov	rax, qword ptr [r8 + 99572]
	mov	qword ptr [rsp + 1184], rax
	vmovss	dword ptr [rsp + 1192], xmm1
	vmovlps	qword ptr [rsp + 1196], xmm9
	mov	dword ptr [rsp + 1204], 0
	mov	rax, qword ptr [r8 + 99620]
	mov	qword ptr [rsp + 1208], rax
	vmovss	dword ptr [rsp + 1216], xmm0
	vmovlps	qword ptr [rsp + 1220], xmm9
	mov	dword ptr [rsp + 1228], 0
	mov	rax, qword ptr [r8 + 99596]
	mov	qword ptr [rsp + 1232], rax
	vmovss	dword ptr [rsp + 1240], xmm7
	vmovlps	qword ptr [rsp + 1244], xmm9
	mov	dword ptr [rsp + 1252], 0
	mov	rax, qword ptr [r8 + 99604]
	mov	qword ptr [rsp + 1256], rax
	vmovss	dword ptr [rsp + 1264], xmm2
	vmovss	dword ptr [rsp + 1268], xmm3
	vmovlps	qword ptr [rsp + 1272], xmm4
	cmp	byte ptr [rsp + 90], 0
	lea	rcx, [r8 + 120356]
	mov	r9d, dword ptr [rsp + 92]
	cmovne	r9d, esi
	lea	rdx, [r8 + 120360]
	lea	rax, [rsp + 92]
	cmove	rcx, rax
	mov	dword ptr [rsp + 1280], r9d
	mov	ecx, dword ptr [rcx]
	mov	dword ptr [rsp + 1284], ecx
	vmovss	dword ptr [rsp + 1288], xmm6
	vmovlps	qword ptr [rsp + 1292], xmm9
	mov	dword ptr [rsp + 1300], 0
	mov	dword ptr [rsp + 872], 0
	mov	ecx, dword ptr [rsp + 376]      # 4-byte Reload
	mov	dword ptr [rsp + 876], ecx
	vmovss	dword ptr [rsp + 880], xmm8
	vmovss	dword ptr [rsp + 884], xmm11
	vmovss	dword ptr [rsp + 888], xmm12
	vmovss	dword ptr [rsp + 892], xmm13
	mov	rcx, qword ptr [r8 + 99436]
	mov	qword ptr [rsp + 896], rcx
	vmovss	dword ptr [rsp + 904], xmm10
	vmovlps	qword ptr [rsp + 908], xmm9
	mov	dword ptr [rsp + 916], 0
	mov	ecx, dword ptr [rsp + 192]      # 4-byte Reload
	mov	dword ptr [rsp + 920], ecx
	mov	ecx, dword ptr [rsp + 184]      # 4-byte Reload
	mov	dword ptr [rsp + 924], ecx
	vmovss	dword ptr [rsp + 928], xmm5
	vmovlps	qword ptr [rsp + 932], xmm9
	mov	dword ptr [rsp + 940], 0
	mov	rcx, qword ptr [r8 + 99556]
	mov	qword ptr [rsp + 944], rcx
	vmovss	dword ptr [rsp + 952], xmm2
	vmovss	dword ptr [rsp + 956], xmm3
	vmovlps	qword ptr [rsp + 960], xmm4
	mov	rcx, qword ptr [r8 + 99588]
	mov	qword ptr [rsp + 968], rcx
	vmovss	dword ptr [rsp + 976], xmm1
	vmovlps	qword ptr [rsp + 980], xmm9
	mov	dword ptr [rsp + 988], 0
	mov	rcx, qword ptr [r8 + 99636]
	mov	qword ptr [rsp + 992], rcx
	vmovss	dword ptr [rsp + 1000], xmm0
	vmovlps	qword ptr [rsp + 1004], xmm9
	mov	dword ptr [rsp + 1012], 0
	mov	rcx, qword ptr [r8 + 99612]
	mov	qword ptr [rsp + 1016], rcx
	vmovss	dword ptr [rsp + 1024], xmm7
	vmovlps	qword ptr [rsp + 1028], xmm9
	mov	r9d, dword ptr [rsp + 136]
	cmovne	r9d, r12d
	mov	dword ptr [rsp + 1036], 0
	lea	rcx, [rsp + 136]
	cmove	rdx, rcx
	mov	dword ptr [rsp + 1040], r9d
	mov	edx, dword ptr [rdx]
	mov	dword ptr [rsp + 1044], edx
	vmovss	dword ptr [rsp + 1048], xmm6
	vmovlps	qword ptr [rsp + 1052], xmm9
	mov	dword ptr [rsp + 1060], 0
	mov	edx, dword ptr [r8 + 99628]
	mov	dword ptr [rsp + 528], edx
	mov	r9d, dword ptr [r8 + 99632]
	mov	dword ptr [rsp + 532], r9d
	vmovss	dword ptr [rsp + 536], xmm8
	vmovss	dword ptr [rsp + 540], xmm11
	vmovss	dword ptr [rsp + 544], xmm12
	vmovss	dword ptr [rsp + 548], xmm13
	mov	dword ptr [rsp + 552], edx
	mov	dword ptr [rsp + 556], r9d
	vmovss	dword ptr [rsp + 560], xmm0
	vmovlps	qword ptr [rsp + 564], xmm9
	mov	dword ptr [rsp + 572], 0
	mov	edx, dword ptr [r8 + 99444]
	mov	dword ptr [rsp + 576], edx
	mov	edx, dword ptr [r8 + 99452]
	mov	dword ptr [rsp + 580], edx
	vmovss	dword ptr [rsp + 584], xmm10
	vmovlps	qword ptr [rsp + 588], xmm9
	mov	dword ptr [rsp + 596], 0
	mov	edx, dword ptr [r8 + 68828]
	mov	dword ptr [rsp + 600], edx
	mov	edx, dword ptr [r8 + 68868]
	mov	dword ptr [rsp + 604], edx
	vmovss	dword ptr [rsp + 608], xmm10
	vmovlps	qword ptr [rsp + 612], xmm9
	mov	dword ptr [rsp + 620], 0
	mov	dword ptr [rsp + 624], r15d
	mov	dword ptr [rsp + 628], r13d
	vmovss	dword ptr [rsp + 632], xmm5
	vmovlps	qword ptr [rsp + 636], xmm9
	mov	dword ptr [rsp + 644], 0
	mov	rdx, qword ptr [r8 + 99548]
	mov	qword ptr [rsp + 648], rdx
	vmovss	dword ptr [rsp + 656], xmm2
	vmovss	dword ptr [rsp + 660], xmm3
	vmovlps	qword ptr [rsp + 664], xmm4
	mov	rdx, qword ptr [r8 + 99580]
	mov	qword ptr [rsp + 672], rdx
	vmovss	dword ptr [rsp + 680], xmm1
	vmovlps	qword ptr [rsp + 684], xmm9
	mov	dword ptr [rsp + 692], 0
	mov	edx, dword ptr [r8 + 68832]
	mov	dword ptr [rsp + 696], edx
	mov	edx, dword ptr [r8 + 68872]
	mov	dword ptr [rsp + 700], edx
	vmovss	dword ptr [rsp + 704], xmm5
	vmovlps	qword ptr [rsp + 708], xmm9
	mov	dword ptr [rsp + 716], 0
	mov	edx, dword ptr [r8 + 68836]
	mov	dword ptr [rsp + 720], edx
	mov	edx, dword ptr [r8 + 68876]
	mov	dword ptr [rsp + 724], edx
	vmovss	dword ptr [rsp + 728], xmm2
	vmovss	dword ptr [rsp + 732], xmm3
	vmovlps	qword ptr [rsp + 736], xmm4
	mov	edx, dword ptr [r8 + 68840]
	mov	dword ptr [rsp + 744], edx
	mov	edx, dword ptr [r8 + 68880]
	mov	dword ptr [rsp + 748], edx
	vmovss	dword ptr [rsp + 752], xmm1
	vmovlps	qword ptr [rsp + 756], xmm9
	mov	dword ptr [rsp + 764], 0
	mov	edx, dword ptr [r8 + 68844]
	mov	dword ptr [rsp + 768], edx
	mov	edx, dword ptr [r8 + 68884]
	mov	dword ptr [rsp + 772], edx
	vmovss	dword ptr [rsp + 776], xmm0
	vmovlps	qword ptr [rsp + 780], xmm9
	mov	dword ptr [rsp + 788], 0
	mov	edx, dword ptr [r8 + 68848]
	mov	dword ptr [rsp + 792], edx
	mov	edx, dword ptr [r8 + 68888]
	mov	dword ptr [rsp + 796], edx
	vmovss	dword ptr [rsp + 800], xmm7
	vmovlps	qword ptr [rsp + 804], xmm9
	mov	dword ptr [rsp + 812], 0
	mov	edx, dword ptr [r8 + 68860]
	mov	dword ptr [rsp + 816], edx
	mov	edx, dword ptr [r8 + 68900]
	mov	dword ptr [rsp + 820], edx
	vmovss	dword ptr [rsp + 824], xmm6
	vmovlps	qword ptr [rsp + 828], xmm9
	mov	dword ptr [rsp + 836], 0
	mov	edx, dword ptr [r8 + 68852]
	mov	dword ptr [rsp + 840], edx
	mov	edx, dword ptr [r8 + 68892]
	mov	dword ptr [rsp + 844], edx
	mov	qword ptr [rsp + 1360], r8
	lea	rdx, [rsp + 256]
	mov	qword ptr [rsp + 1368], rdx
	lea	r9, [rsp + 1392]
	mov	qword ptr [rsp + 1376], r9
	lea	r10, [rsp + 520]
	mov	qword ptr [rsp + 1384], r10
	mov	qword ptr [rsp + 264], r8
	mov	qword ptr [rsp + 272], r9
	lea	r10, [rsp + 260]
	mov	qword ptr [rsp + 280], r10
	lea	r10, [rsp + 1712]
	mov	qword ptr [rsp + 288], r10
	lea	r11, [rsp + 1064]
	mov	qword ptr [rsp + 296], r11
	mov	qword ptr [rsp + 384], r8
	mov	qword ptr [rsp + 392], r9
	mov	r11, qword ptr [rsp + 200]      # 8-byte Reload
	mov	qword ptr [rsp + 400], r11
	mov	qword ptr [rsp + 408], r10
	lea	r11, [rsp + 872]
	mov	qword ptr [rsp + 416], r11
	mov	qword ptr [rsp + 312], r8
	mov	qword ptr [rsp + 320], r9
	mov	qword ptr [rsp + 328], r10
	lea	r9, [rsp + 528]
	mov	qword ptr [rsp + 336], r9
	mov	qword ptr [rsp + 304], rdx
	mov	qword ptr [rsp + 424], rdx
	mov	qword ptr [rsp + 344], rdx
	mov	qword ptr [rsp + 472], r8
	lea	rdx, [rsp + 147]
	mov	qword ptr [rsp + 480], rdx
	lea	rdx, [rsp + 79]
	mov	qword ptr [rsp + 488], rdx
	lea	rdx, [rsp + 91]
	mov	qword ptr [rsp + 496], rdx
	lea	rdx, [rsp + 312]
	mov	qword ptr [rsp + 504], rdx
	mov	qword ptr [rsp + 1304], r8
	lea	rdx, [rsp + 90]
	mov	qword ptr [rsp + 1312], rdx
	lea	rdx, [rsp + 264]
	mov	qword ptr [rsp + 1320], rdx
	mov	qword ptr [rsp + 1328], rax
	lea	rax, [rsp + 472]
	mov	qword ptr [rsp + 1336], rax
	lea	rax, [rsp + 384]
	mov	qword ptr [rsp + 1344], rax
	mov	qword ptr [rsp + 1352], rcx
	vmovsd	xmm0, qword ptr [rip + __xmm@00000000000000003f8000003f800000] # xmm0 = [1.0E+0,1.0E+0,0.0E+0,0.0E+0]
	vmovups	xmmword ptr [rsp + 848], xmm0
	mov	dword ptr [rsp + 256], 1065353216
	cmp	dword ptr [r8 + 20672], 0
	je	.LBB11_221
# %bb.217:
	cmp	byte ptr [r8 + 23628], 1
	jne	.LBB11_221
# %bb.218:
	cmp	dword ptr [rsp + 372], 0        # 4-byte Folded Reload
	je	.LBB11_221
# %bb.219:
	cmp	dword ptr [rsp + 136], 0
	je	.LBB11_221
# %bb.220:
	mov	rax, qword ptr [rsp + 1720]
	cmp	rax, -1
	mov	rdx, qword ptr [rsp + 512]      # 8-byte Reload
	cmovne	rdx, rax
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z"
	mov	r8, qword ptr [rsp + 80]        # 8-byte Reload
	test	al, al
	je	.LBB11_289
.LBB11_221:
	cmp	byte ptr [rsp + 79], 1
	jne	.LBB11_247
# %bb.222:
	mov	r13d, dword ptr [r8 + 99664]
	mov	esi, dword ptr [rsp + 92]
	cmp	r13d, esi
	cmovae	r13d, esi
	mov	r14d, dword ptr [r8 + 99668]
	cmp	r14d, esi
	cmovae	r14d, esi
	cmp	r13d, r14d
	cmova	r14d, r13d
	mov	ebp, dword ptr [r8 + 99672]
	cmp	ebp, esi
	cmovae	ebp, esi
	cmp	r14d, ebp
	cmova	ebp, r14d
	mov	r12d, dword ptr [r8 + 99688]
	cmp	r12d, esi
	cmovae	r12d, esi
	cmp	r12d, ebp
	cmovae	r12d, ebp
	cmp	r14d, r12d
	cmova	r12d, r14d
	mov	ecx, dword ptr [r8 + 99684]
	mov	eax, dword ptr [r8 + 99680]
	cmp	eax, ecx
	cmovb	ecx, eax
	mov	eax, dword ptr [r8 + 99676]
	cmp	eax, ecx
	cmova	ecx, eax
	mov	dword ptr [rsp + 116], ecx      # 4-byte Spill
	mov	eax, dword ptr [r8 + 99572]
	cmp	r13d, eax
	cmovb	eax, r13d
	mov	dword ptr [rsp + 104], eax      # 4-byte Spill
	mov	ecx, dword ptr [r8 + 99576]
	cmp	ecx, esi
	cmovae	ecx, esi
	cmp	ebp, ecx
	cmova	ecx, ebp
	mov	eax, dword ptr [r8 + 99700]
	cmp	eax, esi
	cmovae	eax, esi
	cmp	ecx, eax
	mov	dword ptr [rsp + 128], ecx      # 4-byte Spill
	cmova	eax, ecx
	mov	ecx, dword ptr [r8 + 99704]
	cmp	ecx, esi
	cmovae	ecx, esi
	cmp	eax, ecx
	mov	dword ptr [rsp + 124], eax      # 4-byte Spill
	cmova	ecx, eax
	mov	eax, dword ptr [r8 + 99708]
	cmp	eax, esi
	cmovb	esi, eax
	cmp	ecx, esi
	mov	dword ptr [rsp + 108], ecx      # 4-byte Spill
	cmova	esi, ecx
	mov	ebx, dword ptr [r8 + 99692]
	mov	edi, dword ptr [rsp + 136]
	cmp	ebx, edi
	cmovae	ebx, edi
	mov	eax, dword ptr [r8 + 99696]
	cmp	eax, edi
	cmovae	eax, edi
	cmp	ebx, eax
	cmova	eax, ebx
	mov	ecx, dword ptr [r8 + 99588]
	cmp	ebx, ecx
	cmovb	ecx, ebx
	mov	dword ptr [rsp + 132], ecx      # 4-byte Spill
	mov	r15d, dword ptr [r8 + 99592]
	cmp	r15d, edi
	cmovae	r15d, edi
	cmp	eax, r15d
	mov	dword ptr [rsp + 112], eax      # 4-byte Spill
	cmova	r15d, eax
	mov	ecx, dword ptr [r8 + 99720]
	cmp	ecx, edi
	cmovae	ecx, edi
	cmp	r15d, ecx
	cmova	ecx, r15d
	mov	eax, dword ptr [r8 + 99724]
	cmp	eax, edi
	cmovb	edi, eax
	cmp	ecx, edi
	mov	dword ptr [rsp + 100], ecx      # 4-byte Spill
	cmova	edi, ecx
	mov	r9, qword ptr [rsp + 1712]
	lea	rdx, [rsp + 1392]
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r8, qword ptr [rsp + 200]       # 8-byte Reload
	call	"?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z"
	test	al, al
	je	.LBB11_289
# %bb.223:
	lea	rcx, [rsp + 312]
	xor	edx, edx
	xor	r8d, r8d
	movzx	r9d, byte ptr [rsp + 99]        # 1-byte Folded Reload
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.224:
	lea	rcx, [rsp + 264]
	xor	edx, edx
	mov	r8d, dword ptr [rsp + 104]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.225:
	lea	rcx, [rsp + 472]
	xor	edx, edx
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.226:
	lea	rcx, [rsp + 472]
	mov	dl, 5
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.227:
	lea	rcx, [rsp + 384]
	xor	edx, edx
	mov	r8d, dword ptr [rsp + 132]      # 4-byte Reload
	mov	r9b, 1
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	je	.LBB11_289
# %bb.228:
	mov	edx, dword ptr [rcx + 99628]
	mov	r8d, dword ptr [rcx + 99632]
	lea	rcx, [rsp + 312]
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.229:
	lea	rcx, [rsp + 264]
	mov	edx, r13d
	mov	r8d, r14d
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	je	.LBB11_289
# %bb.230:
	mov	edx, dword ptr [rcx + 99676]
	lea	rcx, [rsp + 312]
	mov	r8d, dword ptr [rsp + 116]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.231:
	lea	rcx, [rsp + 264]
	mov	edx, r14d
	mov	r8d, r12d
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.232:
	lea	rcx, [rsp + 384]
	mov	edx, ebx
	mov	r8d, dword ptr [rsp + 112]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	je	.LBB11_289
# %bb.233:
	mov	r8d, dword ptr [rcx + 99684]
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 116]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.234:
	lea	rcx, [rsp + 264]
	mov	edx, r12d
	mov	r8d, ebp
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.235:
	lea	rcx, [rsp + 264]
	mov	edx, ebp
	mov	r8d, dword ptr [rsp + 128]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.236:
	lea	rcx, [rsp + 472]
	mov	dl, 4
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.237:
	lea	rcx, [rsp + 384]
	mov	edx, dword ptr [rsp + 112]      # 4-byte Reload
	mov	r8d, r15d
	xor	r9d, r9d
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.238:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 124]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 108]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	je	.LBB11_289
# %bb.239:
	mov	edx, dword ptr [rcx + 99712]
	mov	r8d, dword ptr [rcx + 99716]
	lea	rcx, [rsp + 312]
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.240:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 108]      # 4-byte Reload
	mov	r8d, esi
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.241:
	lea	rcx, [rsp + 384]
	mov	edx, dword ptr [rsp + 100]      # 4-byte Reload
	mov	r8d, edi
	xor	r9d, r9d
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.242:
	lea	rcx, [rsp + 1304]
	mov	edx, esi
	mov	r8d, edi
	xor	r9d, r9d
	call	"??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
.LBB11_243:
	cmp	byte ptr [rsp + 364], 0         # 1-byte Folded Reload
	je	.LBB11_291
# %bb.244:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	vmovss	xmm2, dword ptr [rcx + 64016]   # xmm2 = mem[0],zero,zero,zero
	mov	dword ptr [rsp + 40], 0
	mov	dword ptr [rsp + 32], 0
	lea	rdx, [rsp + 256]
	vmovss	xmm3, dword ptr [rip + __real@3f800000] # xmm3 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	call	"?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z"
	test	al, al
	je	.LBB11_289
# %bb.245:
	mov	rax, qword ptr [rsp + 1712]
	vmovss	xmm0, dword ptr [rsp + 260]     # xmm0 = mem[0],zero,zero,zero
	mov	r8d, dword ptr [rsp + 92]
	mov	qword ptr [rsp + 40], rax
	vmovss	dword ptr [rsp + 32], xmm0
	lea	rdx, [rsp + 1392]
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r9d, dword ptr [rsp + 360]      # 4-byte Reload
	call	"?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	test	al, al
	je	.LBB11_289
# %bb.246:
	mov	rax, qword ptr [rsp + 1712]
	mov	r9d, dword ptr [rsp + 136]
	mov	qword ptr [rsp + 48], rax
	mov	eax, dword ptr [rsp + 356]      # 4-byte Reload
	mov	dword ptr [rsp + 32], eax
	mov	byte ptr [rsp + 40], 0
	lea	rdx, [rsp + 1392]
	mov	r8, qword ptr [rsp + 200]       # 8-byte Reload
	call	"?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z"
	mov	dl, 1
	test	al, al
	je	.LBB11_289
	jmp	.LBB11_290
.LBB11_247:
	lea	rcx, [rsp + 264]
	xor	edx, edx
	mov	r8d, dword ptr [rsp + 180]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.248:
	lea	rcx, [rsp + 312]
	xor	edx, edx
	mov	r8d, dword ptr [rsp + 252]      # 4-byte Reload
	movzx	r9d, byte ptr [rsp + 99]        # 1-byte Folded Reload
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.249:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 248]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 244]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.250:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 180]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 240]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.251:
	lea	rcx, [rsp + 472]
	xor	edx, edx
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.252:
	lea	rcx, [rsp + 384]
	xor	edx, edx
	mov	r8d, dword ptr [rsp + 156]      # 4-byte Reload
	mov	r9b, 1
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.253:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 252]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 248]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.254:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 240]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 148]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.255:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 148]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 236]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.256:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 244]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 232]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.257:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 236]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 228]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.258:
	lea	rcx, [rsp + 472]
	mov	dl, 8
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.259:
	mov	edx, dword ptr [rsp + 156]      # 4-byte Reload
	test	edx, edx
	sete	r9b
	lea	rcx, [rsp + 384]
	mov	r8d, dword ptr [rsp + 224]      # 4-byte Reload
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.260:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 228]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 132]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.261:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 232]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 104]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.262:
	mov	edx, dword ptr [rsp + 224]      # 4-byte Reload
	test	edx, edx
	sete	r9b
	lea	rcx, [rsp + 384]
	mov	r8d, dword ptr [rsp + 192]      # 4-byte Reload
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.263:
	cmp	byte ptr [rsp + 89], 0          # 1-byte Folded Reload
	je	.LBB11_269
# %bb.264:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 132]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 220]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.265:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 104]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 112]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.266:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 220]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 116]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.267:
	lea	rcx, [rsp + 472]
	mov	dl, 2
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.268:
	mov	edx, dword ptr [rsp + 192]      # 4-byte Reload
	test	edx, edx
	sete	r9b
	lea	rcx, [rsp + 384]
	mov	r8d, dword ptr [rsp + 184]      # 4-byte Reload
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
.LBB11_269:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 116]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 216]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.270:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 112]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 188]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.271:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 188]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 212]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.272:
	mov	r8d, dword ptr [rsp + 368]      # 4-byte Reload
	mov	eax, dword ptr [rsp + 140]      # 4-byte Reload
	cmp	r8d, eax
	cmovae	r8d, eax
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 216]      # 4-byte Reload
	mov	edi, r8d
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.273:
	lea	rcx, [rsp + 472]
	mov	dl, 1
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.274:
	mov	edx, dword ptr [rsp + 184]      # 4-byte Reload
	test	edx, edx
	sete	r9b
	lea	rcx, [rsp + 384]
	mov	r8d, dword ptr [rsp + 152]      # 4-byte Reload
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.275:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 212]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 124]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.276:
	lea	rcx, [rsp + 264]
	mov	edx, edi
	mov	r8d, dword ptr [rsp + 140]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.277:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 140]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 120]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.278:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 124]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 100]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.279:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 120]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 176]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.280:
	lea	rcx, [rsp + 312]
	mov	edx, dword ptr [rsp + 100]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 196]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.281:
	lea	rcx, [rsp + 472]
	mov	dl, 3
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.282:
	movzx	r9d, byte ptr [rsp + 79]
	lea	rcx, [rsp + 384]
	mov	edx, dword ptr [rsp + 152]      # 4-byte Reload
	mov	r8d, r14d
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.283:
	lea	rcx, [rsp + 264]
	mov	edx, dword ptr [rsp + 176]      # 4-byte Reload
	mov	r8d, ebx
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.284:
	lea	rcx, [rsp + 472]
	mov	dl, 6
	call	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
	test	al, al
	je	.LBB11_289
# %bb.285:
	lea	rcx, [rsp + 384]
	mov	edx, r14d
	mov	r8d, ebp
	xor	r9d, r9d
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.286:
	lea	rcx, [rsp + 264]
	mov	edx, ebx
	mov	r8d, dword ptr [rsp + 128]      # 4-byte Reload
	call	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
	test	al, al
	je	.LBB11_289
# %bb.287:
	lea	rcx, [rsp + 384]
	mov	edx, ebp
	mov	r8d, dword ptr [rsp + 108]      # 4-byte Reload
	xor	r9d, r9d
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	je	.LBB11_289
# %bb.288:
	lea	rcx, [rsp + 1304]
	mov	edx, dword ptr [rsp + 128]      # 4-byte Reload
	mov	r8d, dword ptr [rsp + 108]      # 4-byte Reload
	mov	r9b, 1
	call	"??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	test	al, al
	jne	.LBB11_243
.LBB11_289:
	xor	edx, edx
.LBB11_290:
	lea	rcx, [rsp + 1360]
                                        # kill: def $dl killed $dl killed $edx
	call	"??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z"
                                        # kill: def $al killed $al def $eax
	jmp	.LBB11_6
.LBB11_154:
	mov	rax, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 8]
	mov	rdx, qword ptr [rax + 22880]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	jmp	.LBB11_5
.LBB11_156:
	lea	rcx, [r9 + 23144]
	mov	r8d, dword ptr [r9 + 112]
	mov	rdx, qword ptr [r9 + 8]
	xor	r9d, r9d
	call	"?d3d11_glyph_text_rebuild_title@maicord@@YA_NPEAUD3D11GlyphTextOwner@1@PEAUID3D11DeviceContext@@I_N@Z"
	mov	r9, qword ptr [rsp + 80]        # 8-byte Reload
	mov	rcx, qword ptr [rsp + 200]      # 8-byte Reload
	mov	edx, eax
	xor	eax, eax
	test	dl, dl
	je	.LBB11_6
# %bb.157:
	mov	r8d, ebx
	mov	rax, qword ptr [r9 + 88]
	jmp	.LBB11_21
.LBB11_158:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	movzx	eax, byte ptr [rsp + 89]        # 1-byte Folded Reload
	jne	.LBB11_167
# %bb.159:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rdx, qword ptr [rcx + 61104]
	test	rdx, rdx
	je	.LBB11_164
# %bb.160:
	cmp	byte ptr [rdx + 398], 1
	jne	.LBB11_164
# %bb.161:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB11_166
.LBB11_164:
	xor	esi, esi
	jmp	.LBB11_167
.LBB11_163:
	xor	r9d, r9d
	mov	eax, dword ptr [rsp + 104]      # 4-byte Reload
	mov	r14d, eax
	mov	edx, dword ptr [rsp + 148]      # 4-byte Reload
	mov	r11d, edx
	mov	ebp, edx
	mov	r15d, eax
	jmp	.LBB11_127
.LBB11_166:
	movzx	esi, byte ptr [rsp + 79]
	xor	sil, 1
	jmp	.LBB11_167
.LBB11_291:
	mov	dl, 1
	jmp	.LBB11_290
.LBB11_292:
	mov	rax, qword ptr [rcx + 63896]
	test	rax, rax
	je	.LBB11_61
# %bb.293:
	movzx	ebx, byte ptr [rax + 3909]
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	call	"?d3d11_renderer_interaction_text_state_current@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	byte ptr [rsp + 91], al
	lea	r8, [rcx + 99472]
	test	bl, bl
	lea	rdx, [rsp + 92]
	cmovne	rdx, r8
	mov	r10d, eax
	and	r10b, bl
	mov	r9, qword ptr [rcx + 88]
	mov	r8d, esi
	test	r9, r9
	jne	.LBB11_102
	jmp	.LBB11_103
	.seh_endproc
                                        # -- End function
	.def	"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,12
	.globl	"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB12_7
# %bb.1:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB12_7
# %bb.2:
	mov	al, 1
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB12_5
# %bb.3:
	mov	rcx, qword ptr [rcx + 63976]
	test	rcx, rcx
	je	.LBB12_5
# %bb.4:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
                                        # kill: def $al killed $al def $eax
	xor	al, 1
.LBB12_5:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB12_7:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,13
	.globl	"?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_video_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rax, qword ptr [rcx + 63960]
	test	rax, rax
	je	.LBB13_4
# %bb.1:
	cmp	byte ptr [rax + 849], 0
	jne	.LBB13_2
.LBB13_4:
	mov	rcx, rsi
	call	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB13_2
# %bb.6:
	mov	rcx, rsi
	call	"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB13_2
# %bb.8:
	mov	rax, qword ptr [rsi + 63912]
	test	rax, rax
	je	.LBB13_2
# %bb.9:
	cmp	byte ptr [rax + 2389], 1
	jne	.LBB13_2
# %bb.10:
	cmp	byte ptr [rsi + 120537], 0
	je	.LBB13_14
.LBB13_2:
	xor	eax, eax
.LBB13_3:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB13_14:
	cmp	byte ptr [rsi + 63936], 1
	jne	.LBB13_17
# %bb.15:
	mov	rcx, qword ptr [rsi + 64040]
	test	rcx, rcx
	je	.LBB13_2
# %bb.16:
	call	"?ui_search_scene_current@maicord@@YA_NAEBUUiSearchScene@1@@Z"
	test	al, al
	je	.LBB13_2
.LBB13_17:
	mov	rax, qword ptr [rsi + 66312]
	test	rax, rax
	je	.LBB13_20
# %bb.18:
	cmp	byte ptr [rax + 2040], 0
	jne	.LBB13_2
.LBB13_20:
	mov	rax, qword ptr [rsi + 66328]
	test	rax, rax
	je	.LBB13_23
# %bb.21:
	cmp	byte ptr [rax + 2042], 0
	jne	.LBB13_2
.LBB13_23:
	mov	rax, qword ptr [rsi + 66344]
	test	rax, rax
	je	.LBB13_26
# %bb.24:
	cmp	byte ptr [rax + 1009], 0
	jne	.LBB13_2
.LBB13_26:
	mov	rax, qword ptr [rsi + 66360]
	test	rax, rax
	je	.LBB13_31
# %bb.27:
	cmp	byte ptr [rax + 1392], 0
	jne	.LBB13_2
.LBB13_31:
	mov	rcx, rsi
	call	"?renderer_settings_page_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB13_2
# %bb.33:
	mov	rax, qword ptr [rsi + 64024]
	test	rax, rax
	je	.LBB13_36
# %bb.34:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB13_2
.LBB13_36:
	mov	rax, qword ptr [rsi + 64032]
	test	rax, rax
	je	.LBB13_39
# %bb.37:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB13_2
.LBB13_39:
	mov	rax, qword ptr [rsi + 22736]
	test	rax, rax
	je	.LBB13_42
# %bb.40:
	cmp	byte ptr [rax + 8583], 0
	jne	.LBB13_2
.LBB13_42:
	mov	rax, qword ptr [rsi + 63904]
	test	rax, rax
	je	.LBB13_45
# %bb.43:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB13_2
.LBB13_45:
	mov	rcx, rsi
	call	"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB13_2
# %bb.47:
	mov	rax, qword ptr [rsi + 61104]
	test	rax, rax
	je	.LBB13_50
# %bb.48:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB13_2
.LBB13_50:
	mov	rax, qword ptr [rsi + 63896]
	test	rax, rax
	je	.LBB13_53
# %bb.51:
	cmp	byte ptr [rax + 3909], 0
	jne	.LBB13_2
.LBB13_53:
	mov	rax, qword ptr [rsi + 63880]
	test	rax, rax
	je	.LBB13_57
# %bb.54:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB13_57
# %bb.55:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB13_2
.LBB13_57:
	mov	rcx, qword ptr [rsi + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB13_3
# %bb.58:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB13_3
# %bb.59:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,14
	.globl	"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 72
	.seh_stackalloc 72
	.seh_endprologue
	mov	rdx, qword ptr [rcx + 63944]
	test	rdx, rdx
	je	.LBB14_7
# %bb.1:
	cmp	byte ptr [rdx + 1333], 1
	jne	.LBB14_7
# %bb.2:
	xor	eax, eax
	cmp	qword ptr [rdx + 1312], 0
	je	.LBB14_25
# %bb.3:
	cmp	qword ptr [rcx + 63912], 0
	jne	.LBB14_25
# %bb.4:
	mov	rax, qword ptr [rcx + 63960]
	test	rax, rax
	je	.LBB14_9
# %bb.5:
	cmp	byte ptr [rax + 849], 0
	jne	.LBB14_7
.LBB14_9:
	mov	qword ptr [rsp + 48], rdx       # 8-byte Spill
	mov	qword ptr [rsp + 40], rcx       # 8-byte Spill
	call	"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB14_7
# %bb.11:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	mov	edx, dword ptr [rax + 1320]
	mov	rcx, qword ptr [rax + 1312]
	call	"?ui_widget_manager_resolve@maicord@@YAPEBUUiWidget@1@AEBUUiWidgetManager@1@UUiWidgetHandle@1@@Z"
	test	rax, rax
	je	.LBB14_7
# %bb.12:
	movzx	eax, word ptr [rax + 6]
	and	eax, 9
	cmp	ax, 1
	jne	.LBB14_7
# %bb.13:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	lea	r8, [rax + 1296]
	lea	rdx, [rax + 88]
	lea	rcx, [rsp + 56]
	call	"?ui_rect_intersection@maicord@@YA?AUUiRect@1@AEBU21@0@Z"
	vmovsd	xmm0, qword ptr [rsp + 56]      # xmm0 = mem[0],zero
	vmovsd	xmm1, qword ptr [rsp + 64]      # xmm1 = mem[0],zero
	vcmpltps	xmm0, xmm0, xmm1
	vshufps	xmm0, xmm0, xmm0, 80            # xmm0 = xmm0[0,0,1,1]
	vpcmpeqd	xmm1, xmm1, xmm1
	vtestpd	xmm0, xmm1
	jae	.LBB14_7
# %bb.14:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	movzx	eax, byte ptr [rax + 1331]
	cmp	rax, 3
	ja	.LBB14_7
# %bb.15:
	lea	rcx, [rip + .LJTI14_0]
	movsxd	rax, dword ptr [rcx + 4*rax]
	add	rax, rcx
	jmp	rax
.LBB14_16:
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB14_7
# %bb.33:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	rax, qword ptr [rax + 61072]
	test	rax, rax
	mov	rcx, qword ptr [rsp + 48]       # 8-byte Reload
	je	.LBB14_7
# %bb.34:
	cmp	byte ptr [rax + 633], 1
	jne	.LBB14_7
# %bb.35:
	add	rax, 24
	cmp	qword ptr [rcx + 1312], rax
	jmp	.LBB14_24
.LBB14_19:
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB14_7
# %bb.20:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 66256]
	jmp	.LBB14_23
.LBB14_21:
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB14_7
# %bb.22:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 66328]
.LBB14_23:
	add	rcx, 104
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	cmp	qword ptr [rax + 1312], rcx
.LBB14_24:
	sete	al
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 72
	.seh_endepilogue
	ret
.LBB14_26:
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB14_7
# %bb.27:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 64040]
	call	"?ui_search_scene_message_scene@maicord@@YAPEBUMessageScene@1@AEBUUiSearchScene@1@@Z"
	test	rax, rax
	je	.LBB14_7
# %bb.28:
	cmp	byte ptr [rax + 633], 1
	jne	.LBB14_7
# %bb.29:
	add	rax, 24
	mov	rcx, qword ptr [rsp + 48]       # 8-byte Reload
	cmp	qword ptr [rcx + 1312], rax
	jmp	.LBB14_24
.LBB14_7:
	xor	eax, eax
.LBB14_25:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 72
	.seh_endepilogue
	ret
	.section	.rdata,"dr",associative,"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,15
	.p2align	2, 0x0
.LJTI14_0:
	.long	.LBB14_16-.LJTI14_0
	.long	.LBB14_26-.LJTI14_0
	.long	.LBB14_19-.LJTI14_0
	.long	.LBB14_21-.LJTI14_0
	.section	.text,"xr",discard,"?d3d11_renderer_audio_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,14
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,16
	.globl	"?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_poll_scene_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 88
	.seh_stackalloc 88
	.seh_endprologue
	mov	rax, qword ptr [rcx + 63952]
	test	rax, rax
	je	.LBB15_6
# %bb.1:
	cmp	byte ptr [rax + 2362], 1
	jne	.LBB15_6
# %bb.2:
	cmp	qword ptr [rax + 2304], 0
	je	.LBB15_6
# %bb.3:
	mov	rdx, qword ptr [rcx + 63960]
	test	rdx, rdx
	je	.LBB15_8
# %bb.4:
	cmp	byte ptr [rdx + 849], 0
	jne	.LBB15_6
.LBB15_8:
	mov	qword ptr [rsp + 48], rax       # 8-byte Spill
	mov	qword ptr [rsp + 40], rcx       # 8-byte Spill
	call	"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB15_6
# %bb.11:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	movzx	eax, byte ptr [rax + 2360]
	cmp	eax, 1
	je	.LBB15_15
# %bb.12:
	test	eax, eax
	jne	.LBB15_6
# %bb.13:
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB15_6
# %bb.14:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	rax, qword ptr [rax + 61072]
	jmp	.LBB15_17
.LBB15_15:
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB15_6
# %bb.16:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	mov	rcx, qword ptr [rax + 64040]
	call	"?ui_search_scene_message_scene@maicord@@YAPEBUMessageScene@1@AEBUUiSearchScene@1@@Z"
.LBB15_17:
	test	rax, rax
	je	.LBB15_6
# %bb.18:
	cmp	byte ptr [rax + 633], 1
	jne	.LBB15_6
# %bb.19:
	lea	rcx, [rax + 24]
	mov	rdx, qword ptr [rsp + 48]       # 8-byte Reload
	cmp	qword ptr [rdx + 2304], rcx
	jne	.LBB15_6
# %bb.20:
	mov	qword ptr [rsp + 40], rcx       # 8-byte Spill
	mov	qword ptr [rsp + 56], rax       # 8-byte Spill
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	lea	r8, [rax + 2320]
	lea	rdx, [rax + 120]
	lea	rcx, [rsp + 72]
	call	"?ui_rect_intersection@maicord@@YA?AUUiRect@1@AEBU21@0@Z"
	vmovsd	xmm0, qword ptr [rsp + 72]      # xmm0 = mem[0],zero
	vmovsd	xmm1, qword ptr [rsp + 80]      # xmm1 = mem[0],zero
	vcmpltps	xmm0, xmm0, xmm1
	vshufps	xmm0, xmm0, xmm0, 80            # xmm0 = xmm0[0,0,1,1]
	vpcmpeqd	xmm1, xmm1, xmm1
	vtestpd	xmm0, xmm1
	jae	.LBB15_6
# %bb.21:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	mov	edx, dword ptr [rax + 2312]
	mov	rcx, qword ptr [rsp + 40]       # 8-byte Reload
	call	"?ui_widget_manager_resolve@maicord@@YAPEBUUiWidget@1@AEBUUiWidgetManager@1@UUiWidgetHandle@1@@Z"
	test	rax, rax
	je	.LBB15_6
# %bb.22:
	mov	qword ptr [rsp + 40], rax       # 8-byte Spill
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	mov	edx, dword ptr [rax + 2312]
	mov	rcx, qword ptr [rsp + 56]       # 8-byte Reload
	call	"?message_scene_resolve@maicord@@YAPEBUMessageSceneRowWidget@1@AEBUMessageScene@1@UUiWidgetHandle@1@@Z"
	mov	rdx, rax
	test	rax, rax
	je	.LBB15_24
# %bb.23:
	cmp	byte ptr [rdx + 1274], 0
	je	.LBB15_27
.LBB15_24:
	xor	ecx, ecx
	xor	r8d, r8d
	jmp	.LBB15_34
.LBB15_27:
	mov	qword ptr [rsp + 64], rdx       # 8-byte Spill
	mov	rdx, qword ptr [rdx]
	mov	rax, qword ptr [rsp + 56]       # 8-byte Reload
	mov	rax, qword ptr [rax + 472]
	mov	rcx, qword ptr [rax]
	call	"?message_store_resolve@maicord@@YAPEBUMessageRecord@1@AEBUMessageStore@1@UMessageHandle@1@@Z"
	test	rax, rax
	je	.LBB15_32
# %bb.28:
	mov	r8, rax
	mov	rax, qword ptr [rsp + 56]       # 8-byte Reload
	mov	rax, qword ptr [rax + 16]
	test	rax, rax
	je	.LBB15_31
# %bb.29:
	mov	rcx, qword ptr [rax]
	test	rcx, rcx
	je	.LBB15_31
# %bb.30:
	mov	rdx, qword ptr [r8]
	mov	qword ptr [rsp + 56], r8        # 8-byte Spill
	call	"?message_poll_store_find@maicord@@YAPEBUMessagePollRecord@1@AEBUMessagePollStore@1@_K@Z"
	mov	rcx, rax
	mov	r8, qword ptr [rsp + 56]        # 8-byte Reload
	jmp	.LBB15_33
.LBB15_31:
	xor	ecx, ecx
	jmp	.LBB15_33
.LBB15_32:
	xor	ecx, ecx
	xor	r8d, r8d
.LBB15_33:
	mov	rdx, qword ptr [rsp + 64]       # 8-byte Reload
.LBB15_34:
	mov	rax, qword ptr [rsp + 40]       # 8-byte Reload
	movzx	r9d, word ptr [rax + 6]
	xor	eax, eax
	test	r9b, 1
	je	.LBB15_35
# %bb.36:
	test	rdx, rdx
	je	.LBB15_35
# %bb.37:
	and	r9d, 8
	jne	.LBB15_35
# %bb.38:
	movzx	r9d, word ptr [rdx + 56]
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	cmp	r9w, word ptr [rax + 2312]
	jne	.LBB15_6
# %bb.39:
	xor	eax, eax
	test	rcx, rcx
	je	.LBB15_35
# %bb.40:
	mov	r9, qword ptr [rsp + 48]        # 8-byte Reload
	movzx	r9d, word ptr [r9 + 2314]
	cmp	word ptr [rdx + 58], r9w
	jne	.LBB15_35
# %bb.41:
	mov	rdx, qword ptr [rcx + 8]
	cmp	rdx, qword ptr [r8 + 8]
	jne	.LBB15_6
# %bb.42:
	mov	rdx, qword ptr [rsp + 48]       # 8-byte Reload
	mov	rdx, qword ptr [rdx]
	cmp	rdx, qword ptr [r8]
	jne	.LBB15_6
# %bb.43:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	mov	eax, dword ptr [rax + 24]
	cmp	eax, dword ptr [rcx + 440]
	jne	.LBB15_6
# %bb.44:
	mov	rax, qword ptr [rsp + 48]       # 8-byte Reload
	mov	eax, dword ptr [rax + 28]
	cmp	eax, dword ptr [rcx + 444]
	sete	al
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 88
	.seh_endepilogue
	ret
.LBB15_6:
	xor	eax, eax
.LBB15_35:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 88
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,17
	.globl	"?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_poll_voters_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rdx, qword ptr [rcx + 63960]
	test	rdx, rdx
	je	.LBB16_6
# %bb.1:
	cmp	byte ptr [rdx + 849], 1
	jne	.LBB16_6
# %bb.2:
	mov	rax, qword ptr [rdx + 648]
	test	rax, rax
	je	.LBB16_6
# %bb.3:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	edx, dword ptr [rdx + 656]
	mov	rcx, rax
	call	"?ui_widget_manager_resolve@maicord@@YAPEBUUiWidget@1@AEBUUiWidgetManager@1@UUiWidgetHandle@1@@Z"
	test	rax, rax
	je	.LBB16_6
# %bb.4:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	call	"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB16_11
.LBB16_6:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB16_11:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	call	"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
                                        # kill: def $al killed $al def $eax
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,18
	.globl	"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_tooltip_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	cmp	qword ptr [rcx + 63984], 0
	je	.LBB17_28
# %bb.1:
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB17_28
# %bb.2:
	mov	rax, qword ptr [rcx + 63960]
	test	rax, rax
	je	.LBB17_4
# %bb.3:
	cmp	byte ptr [rax + 849], 0
	jne	.LBB17_28
.LBB17_4:
	mov	rax, qword ptr [rcx + 66312]
	test	rax, rax
	je	.LBB17_6
# %bb.5:
	cmp	byte ptr [rax + 2040], 0
	jne	.LBB17_28
.LBB17_6:
	mov	rax, qword ptr [rcx + 66328]
	test	rax, rax
	je	.LBB17_8
# %bb.7:
	cmp	byte ptr [rax + 2042], 0
	jne	.LBB17_28
.LBB17_8:
	mov	rax, qword ptr [rcx + 66344]
	test	rax, rax
	je	.LBB17_10
# %bb.9:
	cmp	byte ptr [rax + 1009], 0
	jne	.LBB17_28
.LBB17_10:
	mov	rax, qword ptr [rcx + 66360]
	test	rax, rax
	je	.LBB17_12
# %bb.11:
	cmp	byte ptr [rax + 1392], 0
	jne	.LBB17_28
.LBB17_12:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB17_16
# %bb.13:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB17_16
# %bb.14:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB17_28
# %bb.15:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	je	.LBB17_28
.LBB17_16:
	mov	rax, qword ptr [rcx + 64032]
	test	rax, rax
	je	.LBB17_18
# %bb.17:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB17_28
.LBB17_18:
	mov	rax, qword ptr [rcx + 61104]
	test	rax, rax
	je	.LBB17_20
# %bb.19:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB17_28
.LBB17_20:
	mov	rax, qword ptr [rcx + 63896]
	test	rax, rax
	je	.LBB17_22
# %bb.21:
	cmp	byte ptr [rax + 3909], 0
	jne	.LBB17_28
.LBB17_22:
	mov	rax, qword ptr [rcx + 63880]
	test	rax, rax
	je	.LBB17_25
# %bb.23:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB17_25
# %bb.24:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB17_28
.LBB17_25:
	mov	rax, qword ptr [rcx + 63888]
	test	rax, rax
	je	.LBB17_29
# %bb.26:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB17_29
# %bb.27:
	cmp	byte ptr [rax + 1428], 0
	je	.LBB17_29
.LBB17_28:
	xor	eax, eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB17_29:
	mov	rcx, qword ptr [rcx + 63984]
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	jmp	"?ui_tooltip_visible@maicord@@YA_NAEBUUiTooltipScene@1@@Z" # TAILCALL
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,19
	.globl	"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_search_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 64040]
	test	rcx, rcx
	je	.LBB18_4
# %bb.1:
	call	"?ui_search_scene_current@maicord@@YA_NAEBUUiSearchScene@1@@Z"
	test	al, al
	je	.LBB18_4
# %bb.2:
	cmp	byte ptr [rsi + 120537], 0
	je	.LBB18_7
.LBB18_4:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB18_7:
	mov	rax, qword ptr [rsi + 63968]
	test	rax, rax
	je	.LBB18_11
# %bb.8:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB18_11
# %bb.9:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB18_4
# %bb.10:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB18_4
.LBB18_11:
	mov	rax, qword ptr [rsi + 64032]
	test	rax, rax
	je	.LBB18_14
# %bb.12:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB18_4
.LBB18_14:
	mov	rax, qword ptr [rsi + 64024]
	test	rax, rax
	je	.LBB18_16
# %bb.15:
	movzx	eax, byte ptr [rax + 1101]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB18_16:
	mov	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,20
	.globl	"?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_unread_banner_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	cmp	qword ptr [rcx + 63992], 0
	je	.LBB19_19
# %bb.1:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB19_2
.LBB19_19:
	xor	eax, eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB19_2:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB19_7
# %bb.3:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB19_7
# %bb.4:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB19_19
# %bb.5:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	je	.LBB19_19
# %bb.6:
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB19_9
.LBB19_7:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB19_9
# %bb.8:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB19_19
.LBB19_9:
	mov	rax, qword ptr [rcx + 64024]
	test	rax, rax
	je	.LBB19_11
# %bb.10:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB19_19
.LBB19_11:
	mov	rax, qword ptr [rcx + 64032]
	test	rax, rax
	je	.LBB19_13
# %bb.12:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB19_19
.LBB19_13:
	mov	rax, qword ptr [rcx + 63904]
	test	rax, rax
	je	.LBB19_15
# %bb.14:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB19_19
.LBB19_15:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	call	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB19_19
# %bb.16:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	call	"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB19_19
# %bb.17:
	mov	rax, qword ptr [rcx + 61072]
	test	rax, rax
	je	.LBB19_19
# %bb.18:
	cmp	byte ptr [rax + 633], 1
	jne	.LBB19_19
# %bb.20:
	mov	rcx, qword ptr [rcx + 63992]
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	jmp	"?ui_unread_banner_visible@maicord@@YA_NAEBUUiUnreadBannerScene@1@@Z" # TAILCALL
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,21
	.globl	"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_pinned_messages_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	call	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB20_1
# %bb.3:
	mov	rcx, qword ptr [rsi + 66256]
	test	rcx, rcx
	je	.LBB20_1
# %bb.4:
	call	"?ui_pinned_messages_scene_current@maicord@@YA_NAEBUUiPinnedMessagesScene@1@@Z"
	test	al, al
	je	.LBB20_1
# %bb.5:
	cmp	byte ptr [rsi + 120537], 0
	jne	.LBB20_1
# %bb.9:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB20_12
# %bb.10:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB20_1
.LBB20_12:
	mov	rax, qword ptr [rsi + 63968]
	test	rax, rax
	je	.LBB20_18
# %bb.13:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB20_18
# %bb.14:
	cmp	byte ptr [rsi + 120537], 0
	jne	.LBB20_1
# %bb.16:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB20_1
# %bb.17:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB20_1
.LBB20_18:
	mov	rax, qword ptr [rsi + 64024]
	test	rax, rax
	je	.LBB20_21
# %bb.19:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB20_1
.LBB20_21:
	mov	rax, qword ptr [rsi + 64032]
	test	rax, rax
	je	.LBB20_24
# %bb.22:
	cmp	byte ptr [rax + 1101], 0
	je	.LBB20_24
.LBB20_1:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB20_24:
	mov	rax, qword ptr [rsi + 63904]
	test	rax, rax
	je	.LBB20_26
# %bb.25:
	movzx	eax, byte ptr [rax + 2638]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB20_26:
	mov	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,22
	.globl	"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 66312]
	test	rcx, rcx
	je	.LBB21_2
# %bb.1:
	call	"?ui_quick_switcher_scene_current@maicord@@YA_NAEBUUiQuickSwitcherScene@1@@Z"
	test	al, al
	je	.LBB21_2
# %bb.3:
	mov	rcx, rsi
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	jmp	"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # TAILCALL
.LBB21_2:
	xor	eax, eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,23
	.globl	"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_inbox_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 66272]
	test	rcx, rcx
	je	.LBB22_3
# %bb.1:
	call	"?ui_inbox_scene_current@maicord@@YA_NAEBUUiInboxScene@1@@Z"
	test	al, al
	je	.LBB22_3
# %bb.2:
	cmp	byte ptr [rsi + 120537], 0
	je	.LBB22_7
.LBB22_3:
	xor	eax, eax
.LBB22_4:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB22_7:
	mov	rax, qword ptr [rsi + 63968]
	test	rax, rax
	je	.LBB22_12
# %bb.8:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB22_12
# %bb.9:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB22_3
# %bb.10:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB22_3
# %bb.11:
	cmp	byte ptr [rsi + 120537], 0
	jne	.LBB22_15
.LBB22_12:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB22_15
# %bb.13:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB22_3
.LBB22_15:
	mov	rax, qword ptr [rsi + 22736]
	test	rax, rax
	je	.LBB22_18
# %bb.16:
	cmp	byte ptr [rax + 8583], 0
	jne	.LBB22_3
.LBB22_18:
	mov	rax, qword ptr [rsi + 63904]
	test	rax, rax
	je	.LBB22_21
# %bb.19:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB22_3
.LBB22_21:
	mov	rax, qword ptr [rsi + 61104]
	test	rax, rax
	je	.LBB22_24
# %bb.22:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB22_3
.LBB22_24:
	mov	rcx, rsi
	call	"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB22_3
# %bb.28:
	mov	rax, qword ptr [rsi + 63880]
	test	rax, rax
	je	.LBB22_32
# %bb.29:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB22_32
# %bb.30:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB22_3
.LBB22_32:
	mov	rcx, qword ptr [rsi + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB22_4
# %bb.33:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB22_4
# %bb.34:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,24
	.globl	"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_message_forward_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 66328]
	test	rcx, rcx
	je	.LBB23_2
# %bb.1:
	call	"?ui_message_forward_scene_current@maicord@@YA_NAEBUUiMessageForwardScene@1@@Z"
	test	al, al
	je	.LBB23_2
# %bb.3:
	mov	rcx, rsi
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	jmp	"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # TAILCALL
.LBB23_2:
	xor	eax, eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,25
	.globl	"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 66344]
	test	rcx, rcx
	je	.LBB24_2
# %bb.1:
	call	"?ui_download_scene_current@maicord@@YA_NAEBUUiDownloadScene@1@@Z"
	test	al, al
	je	.LBB24_2
# %bb.3:
	mov	rcx, rsi
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	jmp	"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # TAILCALL
.LBB24_2:
	xor	eax, eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,26
	.globl	"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_shortcuts_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 66360]
	test	rcx, rcx
	je	.LBB25_3
# %bb.1:
	call	"?ui_shortcuts_scene_current@maicord@@YA_NAEBUUiShortcutsScene@1@@Z"
	test	al, al
	je	.LBB25_3
# %bb.2:
	cmp	byte ptr [rsi + 120537], 0
	je	.LBB25_7
.LBB25_3:
	xor	eax, eax
.LBB25_4:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB25_7:
	mov	rax, qword ptr [rsi + 63968]
	test	rax, rax
	je	.LBB25_12
# %bb.8:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB25_12
# %bb.9:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB25_3
# %bb.10:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB25_3
# %bb.11:
	cmp	byte ptr [rsi + 120537], 0
	jne	.LBB25_15
.LBB25_12:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB25_15
# %bb.13:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB25_3
.LBB25_15:
	mov	rax, qword ptr [rsi + 63904]
	test	rax, rax
	je	.LBB25_18
# %bb.16:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB25_3
.LBB25_18:
	mov	rax, qword ptr [rsi + 63880]
	test	rax, rax
	je	.LBB25_22
# %bb.19:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB25_22
# %bb.20:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB25_3
.LBB25_22:
	mov	rcx, qword ptr [rsi + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB25_4
# %bb.23:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB25_4
# %bb.24:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,27
	.globl	"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	call	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB26_1
# %bb.3:
	cmp	byte ptr [rsi + 120537], 0
	jne	.LBB26_7
# %bb.4:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB26_7
# %bb.5:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB26_1
.LBB26_7:
	mov	rcx, qword ptr [rsi + 66448]
	test	rcx, rcx
	je	.LBB26_1
# %bb.8:
	call	"?ui_media_picker_scene_current@maicord@@YA_NAEBUUiMediaPickerScene@1@@Z"
	test	al, al
	je	.LBB26_1
# %bb.9:
	cmp	byte ptr [rsi + 120537], 0
	je	.LBB26_13
.LBB26_1:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB26_13:
	mov	rax, qword ptr [rsi + 63968]
	test	rax, rax
	je	.LBB26_17
# %bb.14:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB26_17
# %bb.15:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB26_1
# %bb.16:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB26_1
.LBB26_17:
	mov	rax, qword ptr [rsi + 64024]
	test	rax, rax
	je	.LBB26_20
# %bb.18:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB26_1
.LBB26_20:
	mov	rax, qword ptr [rsi + 64032]
	test	rax, rax
	je	.LBB26_23
# %bb.21:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB26_1
.LBB26_23:
	mov	rax, qword ptr [rsi + 63904]
	test	rax, rax
	je	.LBB26_25
# %bb.24:
	movzx	eax, byte ptr [rax + 2638]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB26_25:
	mov	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,28
	.globl	"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_call_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rax, qword ptr [rcx + 66376]
	test	rax, rax
	je	.LBB27_4
# %bb.1:
	cmp	byte ptr [rax + 25128], 1
	jne	.LBB27_4
# %bb.2:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB27_7
.LBB27_4:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB27_7:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB27_12
# %bb.8:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB27_12
# %bb.9:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB27_4
# %bb.10:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB27_4
# %bb.11:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB27_15
.LBB27_12:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB27_15
# %bb.13:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB27_4
.LBB27_15:
	mov	rax, qword ptr [rcx + 64032]
	test	rax, rax
	je	.LBB27_18
# %bb.16:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB27_4
.LBB27_18:
	call	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
                                        # kill: def $al killed $al def $eax
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,29
	.globl	"?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_stream_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rax, qword ptr [rcx + 66392]
	test	rax, rax
	je	.LBB28_4
# %bb.1:
	cmp	byte ptr [rax + 432], 1
	jne	.LBB28_4
# %bb.2:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB28_6
.LBB28_4:
	xor	eax, eax
.LBB28_30:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB28_6:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB28_11
# %bb.7:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB28_11
# %bb.8:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB28_4
# %bb.9:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB28_4
# %bb.10:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB28_14
.LBB28_11:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB28_14
# %bb.12:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB28_4
.LBB28_14:
	mov	rax, qword ptr [rcx + 22736]
	test	rax, rax
	je	.LBB28_17
# %bb.15:
	cmp	byte ptr [rax + 8583], 0
	jne	.LBB28_4
.LBB28_17:
	mov	rax, qword ptr [rcx + 63904]
	test	rax, rax
	je	.LBB28_20
# %bb.18:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB28_4
.LBB28_20:
	mov	rax, qword ptr [rcx + 61104]
	test	rax, rax
	je	.LBB28_23
# %bb.21:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB28_4
.LBB28_23:
	mov	rax, qword ptr [rcx + 63880]
	test	rax, rax
	je	.LBB28_27
# %bb.24:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB28_27
# %bb.25:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB28_4
.LBB28_27:
	mov	rcx, qword ptr [rcx + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB28_30
# %bb.28:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB28_30
# %bb.29:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,30
	.globl	"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_guild_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rax, qword ptr [rcx + 66408]
	test	rax, rax
	je	.LBB29_4
# %bb.1:
	cmp	byte ptr [rax + 432], 1
	jne	.LBB29_4
# %bb.2:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB29_6
.LBB29_4:
	xor	eax, eax
.LBB29_27:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB29_6:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB29_11
# %bb.7:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB29_11
# %bb.8:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB29_4
# %bb.9:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB29_4
# %bb.10:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB29_14
.LBB29_11:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB29_14
# %bb.12:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB29_4
.LBB29_14:
	mov	rax, qword ptr [rcx + 63904]
	test	rax, rax
	je	.LBB29_17
# %bb.15:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB29_4
.LBB29_17:
	mov	rax, qword ptr [rcx + 61104]
	test	rax, rax
	je	.LBB29_20
# %bb.18:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB29_4
.LBB29_20:
	mov	rax, qword ptr [rcx + 63880]
	test	rax, rax
	je	.LBB29_24
# %bb.21:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB29_24
# %bb.22:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB29_4
.LBB29_24:
	mov	rcx, qword ptr [rcx + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB29_27
# %bb.25:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB29_27
# %bb.26:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,31
	.globl	"?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_expression_card_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rax, qword ptr [rcx + 66424]
	test	rax, rax
	je	.LBB30_4
# %bb.1:
	cmp	byte ptr [rax + 432], 1
	jne	.LBB30_4
# %bb.2:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB30_6
.LBB30_4:
	xor	eax, eax
.LBB30_27:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB30_6:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB30_11
# %bb.7:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB30_11
# %bb.8:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB30_4
# %bb.9:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB30_4
# %bb.10:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB30_14
.LBB30_11:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB30_14
# %bb.12:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB30_4
.LBB30_14:
	mov	rax, qword ptr [rcx + 63904]
	test	rax, rax
	je	.LBB30_17
# %bb.15:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB30_4
.LBB30_17:
	mov	rax, qword ptr [rcx + 61104]
	test	rax, rax
	je	.LBB30_20
# %bb.18:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB30_4
.LBB30_20:
	mov	rax, qword ptr [rcx + 63880]
	test	rax, rax
	je	.LBB30_24
# %bb.21:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB30_24
# %bb.22:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB30_4
.LBB30_24:
	mov	rcx, qword ptr [rcx + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB30_27
# %bb.25:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB30_27
# %bb.26:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z",unique,32
	.p2align	4                               # -- Begin function ?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z
"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z": # @"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
.seh_proc "?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rdx, qword ptr [rcx + 63968]
	test	rdx, rdx
	je	.LBB31_6
# %bb.1:
	cmp	byte ptr [rdx + 636], 1
	jne	.LBB31_6
# %bb.2:
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB31_10
# %bb.3:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB31_10
# %bb.4:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB31_6
# %bb.9:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	mov	rdx, qword ptr [rcx + 63968]
.LBB31_10:
	cmp	byte ptr [rdx + 668], 0
	je	.LBB31_12
.LBB31_6:
	xor	eax, eax
	and	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB31_12:
	vmovss	xmm0, dword ptr [rcx + 99736]   # xmm0 = mem[0],zero,zero,zero
	vxorps	xmm1, xmm1, xmm1
	vcmpless	xmm1, xmm1, xmm0
	vcmpltss	xmm0, xmm0, dword ptr [rip + __real@3f800000]
	vandps	xmm0, xmm1, xmm0
	vmovd	eax, xmm0
	and	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z",unique,33
	.p2align	4                               # -- Begin function ?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z
"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z": # @"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
.seh_proc "?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	rdx, qword ptr [rcx + 63968]
	test	rdx, rdx
	je	.LBB32_6
# %bb.1:
	cmp	byte ptr [rdx + 636], 1
	jne	.LBB32_6
# %bb.2:
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB32_10
# %bb.3:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB32_10
# %bb.4:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB32_9
.LBB32_6:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB32_9:
	mov	rax, qword ptr [rsp + 32]       # 8-byte Reload
	mov	rdx, qword ptr [rax + 63968]
.LBB32_10:
	cmp	byte ptr [rdx + 668], 0
	setne	al
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__xmm@00010000000000040000000200005010 # -- Begin function ?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z
	.section	.rdata,"dr",discard,__xmm@00010000000000040000000200005010
	.p2align	4, 0x0
__xmm@00010000000000040000000200005010:
	.long	20496                           # 0x5010
	.long	2                               # 0x2
	.long	4                               # 0x4
	.long	65536                           # 0x10000
	.globl	__xmm@000000000000000042c00000447a0000
	.section	.rdata,"dr",discard,__xmm@000000000000000042c00000447a0000
	.p2align	4, 0x0
__xmm@000000000000000042c00000447a0000:
	.long	0x447a0000                      # float 1000
	.long	0x42c00000                      # float 96
	.zero	4
	.zero	4
	.section	.text,"xr",one_only,"?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z",unique,34
	.p2align	4
"?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z": # @"?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z"
.seh_proc "?prepare_text_effects@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@_K@Z"
# %bb.0:
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 104
	.seh_stackalloc 104
	vmovaps	xmmword ptr [rsp + 80], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 80
	.seh_endprologue
	mov	rbx, rdx
	mov	rdi, rcx
	lea	rsi, [rcx + 20664]
	cmp	qword ptr [rcx + 20664], 0
	je	.LBB33_2
# %bb.1:
	movzx	r14d, byte ptr [rdi + 20678]
	jmp	.LBB33_4
.LBB33_2:
	mov	qword ptr [rsp + 64], 0
	vmovaps	xmm0, xmmword ptr [rip + __xmm@00010000000000040000000200005010] # xmm0 = [20496,2,4,65536]
	vmovaps	xmmword ptr [rsp + 48], xmm0
	mov	rcx, qword ptr [rdi]
	mov	rax, qword ptr [rcx]
	xor	r14d, r14d
	lea	rdx, [rsp + 48]
	xor	r8d, r8d
	mov	r9, rsi
	call	qword ptr [rax + 24]
	test	eax, eax
	js	.LBB33_10
# %bb.3:
	mov	byte ptr [rdi + 20678], 0
.LBB33_4:
	mov	rax, rbx
	shr	rax, 5
	movabs	rcx, 314824432191309681
	mul	rcx
	shr	edx, 5
	imul	eax, edx, 60000
	sub	ebx, eax
	vcvtsi2ss	xmm0, xmm5, ebx
	mov	eax, dword ptr [rdi + 112]
	vcvtsi2ss	xmm1, xmm5, rax
	vinsertps	xmm0, xmm0, xmm1, 16    # xmm0 = xmm0[0],xmm1[0],xmm0[2,3]
	vdivps	xmm6, xmm0, xmmword ptr [rip + __xmm@000000000000000042c00000447a0000]
	cmp	dword ptr [rdi + 20672], r14d
	ja	.LBB33_7
# %bb.5:
	vmovsd	xmm0, qword ptr [rdi + 164]     # xmm0 = mem[0],zero
	vcmpneqps	xmm0, xmm6, xmm0
	vshufps	xmm0, xmm0, xmm0, 80            # xmm0 = xmm0[0,0,1,1]
	vmovmskpd	eax, xmm0
	mov	ecx, eax
	shr	cl
	jne	.LBB33_7
# %bb.6:
	and	al, byte ptr [rdi + 20676]
	je	.LBB33_9
.LBB33_7:
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 48], xmm0
	mov	rcx, qword ptr [rdi + 8]
	mov	rdx, qword ptr [rdi + 20664]
	mov	rax, qword ptr [rcx]
	lea	r8, [rsp + 48]
	mov	qword ptr [rsp + 40], r8
	mov	dword ptr [rsp + 32], 0
	xor	r8d, r8d
	mov	r9d, 4
	call	qword ptr [rax + 112]
	test	eax, eax
	js	.LBB33_10
# %bb.8:
	lea	rdx, [rdi + 164]
	vmovlps	qword ptr [rdi + 164], xmm6
	mov	rcx, qword ptr [rsp + 48]
	mov	eax, dword ptr [rdi + 20672]
	shl	rax, 5
	lea	r8, [rax + 4*rax]
	add	r8, 16
	call	memcpy
	mov	rcx, qword ptr [rdi + 8]
	mov	rdx, qword ptr [rdi + 20664]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	movzx	eax, byte ptr [rdi + 20672]
	mov	byte ptr [rdi + 20678], al
.LBB33_9:
	mov	rcx, qword ptr [rdi + 8]
	mov	rax, qword ptr [rcx]
	mov	edx, 1
	mov	r8d, 1
	mov	r9, rsi
	call	qword ptr [rax + 56]
	mov	al, 1
	jmp	.LBB33_11
.LBB33_10:
	xor	eax, eax
.LBB33_11:
                                        # kill: def $al killed $al killed $eax
	vmovaps	xmm6, xmmword ptr [rsp + 80]    # 16-byte Reload
	.seh_startepilogue
	add	rsp, 104
	pop	rbx
	pop	rdi
	pop	rsi
	pop	r14
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__real@3d0f5c29                 # -- Begin function ?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z
	.section	.rdata,"dr",discard,__real@3d0f5c29
	.p2align	2, 0x0
__real@3d0f5c29:
	.long	0x3d0f5c29                      # float 0.0350000001
	.globl	__ymm@00000000000000003f8000003f8000003f800000000000000000000000000000
	.section	.rdata,"dr",discard,__ymm@00000000000000003f8000003f8000003f800000000000000000000000000000
	.p2align	5, 0x0
__ymm@00000000000000003f8000003f8000003f800000000000000000000000000000:
	.zero	4
	.long	0x00000000                      # float 0
	.long	0x00000000                      # float 0
	.long	0x3f800000                      # float 1
	.long	0x3f800000                      # float 1
	.long	0x3f800000                      # float 1
	.long	0x00000000                      # float 0
	.long	0x00000000                      # float 0
	.section	.text,"xr",one_only,"?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z",unique,35
	.p2align	4
"?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z": # @"?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z"
.seh_proc "?draw_settings_backdrop@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@PEBUtagRECT@@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 160
	.seh_stackalloc 160
	.seh_endprologue
	mov	rdi, r9
	mov	r14, r8
	mov	rbx, rdx
	mov	rsi, rcx
	call	"?settings_layer_entrance@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	mov	ebp, eax
	mov	rcx, rsi
	call	"?settings_modal_visible@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@@Z"
	mov	ecx, eax
	or	cl, bpl
	mov	al, 1
	cmp	cl, 1
	jne	.LBB34_12
# %bb.1:
	cmp	byte ptr [rsi + 120312], 0
	je	.LBB34_2
# %bb.4:
	test	bpl, bpl
	je	.LBB34_6
# %bb.5:
	vmovsd	xmm0, qword ptr [r14]           # xmm0 = mem[0],zero
	vmovsd	qword ptr [rsp + 56], xmm0
	vmovss	xmm0, dword ptr [r14 + 8]       # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 64], xmm0
	mov	dword ptr [rsp + 68], 1065353216
	lea	rdx, [rsp + 56]
	mov	rcx, rsi
	call	"?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z"
.LBB34_6:
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 96], xmm0
	mov	rcx, qword ptr [rsi + 8]
	mov	rdx, qword ptr [rsi + 99768]
	mov	rax, qword ptr [rcx]
	lea	r8, [rsp + 96]
	mov	qword ptr [rsp + 40], r8
	mov	dword ptr [rsp + 32], 0
	xor	r8d, r8d
	mov	r9d, 4
	call	qword ptr [rax + 112]
	test	eax, eax
	setns	dl
	mov	rcx, qword ptr [rsp + 96]
	test	rcx, rcx
	setne	al
	and	al, dl
	cmp	al, 1
	jne	.LBB34_12
# %bb.7:
	mov	r15d, eax
	mov	eax, dword ptr [rsi + 112]
	cmp	eax, 1
	adc	eax, 0
	vcvtsi2ss	xmm0, xmm5, rax
	mov	eax, dword ptr [rsi + 104]
	vcvtsi2ss	xmm1, xmm5, rax
	mov	eax, dword ptr [rsi + 108]
	vcvtsi2ss	xmm2, xmm5, rax
	lea	r12, [rsi + 99768]
	vmovss	xmm3, dword ptr [rip + __real@42c00000] # xmm3 = [9.6E+1,0.0E+0,0.0E+0,0.0E+0]
	vdivss	xmm3, xmm3, xmm0
	vmulss	xmm0, xmm3, xmm1
	vmulss	xmm1, xmm3, xmm2
	vxorps	xmm2, xmm2, xmm2
	test	bpl, bpl
	je	.LBB34_9
# %bb.8:
	vmovss	xmm2, dword ptr [rsi + 99736]   # xmm2 = mem[0],zero,zero,zero
	vmulss	xmm2, xmm2, dword ptr [rip + __real@3d0f5c29]
.LBB34_9:
	vmovss	xmm3, dword ptr [rip + __real@3f800000] # xmm3 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vsubss	xmm3, xmm3, xmm2
	vmulss	xmm4, xmm1, xmm3
	vblendps	ymm5, ymm4, ymmword ptr [rip + __ymm@00000000000000003f8000003f8000003f800000000000000000000000000000], 254 # ymm5 = ymm4[0],mem[1,2,3,4,5,6,7]
	vmovups	ymmword ptr [rcx + 12], ymm5
	vmulss	xmm5, xmm0, xmm2
	vmulss	xmm1, xmm1, xmm2
	vmovss	dword ptr [rcx], xmm5
	vmovss	dword ptr [rcx + 4], xmm1
	vmulss	xmm0, xmm0, xmm3
	vmovss	dword ptr [rcx + 8], xmm0
	vinsertps	xmm0, xmm1, xmm0, 28    # xmm0 = xmm1[0],xmm0[0],zero,zero
	vxorps	xmm1, xmm1, xmm1
	vblendps	xmm1, xmm1, xmm5, 1             # xmm1 = xmm5[0],xmm1[1,2,3]
	vshufps	xmm0, xmm1, xmm0, 65            # xmm0 = xmm1[1,0],xmm0[0,1]
	vmovups	xmmword ptr [rcx + 44], xmm0
	vmovss	dword ptr [rcx + 60], xmm4
	mov	rcx, qword ptr [rsi + 8]
	mov	rdx, qword ptr [rsi + 99768]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	vzeroupper
	call	qword ptr [rax + 120]
	mov	dword ptr [rsp + 52], 64
	mov	dword ptr [rsp + 48], 0
	mov	r14, qword ptr [rsi + 8]
	mov	qword ptr [rbx + 16], 0
	lea	r8, [rsi + 56]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	mov	edx, 1
	xor	r9d, r9d
	call	qword ptr [rax + 264]
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 144], xmm0
	mov	rdx, qword ptr [rsi + 22904]
	mov	rax, qword ptr [r14]
	lea	r8, [rsp + 144]
	mov	rcx, r14
	mov	r9d, -1
	call	qword ptr [rax + 280]
	mov	rdx, qword ptr [rsi + 22896]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	call	qword ptr [rax + 344]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	mov	edx, 5
	call	qword ptr [rax + 192]
	mov	rdx, qword ptr [rsi + 22944]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	call	qword ptr [rax + 136]
	mov	rax, qword ptr [r14]
	lea	rcx, [rsp + 48]
	mov	qword ptr [rsp + 40], rcx
	lea	rcx, [rsp + 52]
	mov	qword ptr [rsp + 32], rcx
	mov	rcx, r14
	xor	edx, edx
	mov	r8d, 1
	mov	r9, r12
	call	qword ptr [rax + 144]
	mov	rdx, qword ptr [rsi + 22936]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 88]
	lea	r9, [rsi + 22888]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	xor	edx, edx
	mov	r8d, 1
	call	qword ptr [rax + 56]
	mov	rdx, qword ptr [rsi + 22928]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 72]
	mov	rax, qword ptr [rsi + 99752]
	mov	qword ptr [rsp + 56], rax
	vxorps	xmm0, xmm0, xmm0
	vmovups	xmmword ptr [rsp + 64], xmm0
	mov	rax, qword ptr [r14]
	lea	r9, [rsp + 56]
	mov	rcx, r14
	xor	edx, edx
	mov	r8d, 3
	call	qword ptr [rax + 64]
	lea	r9, [rsi + 22976]
	mov	rax, qword ptr [r14]
	mov	rcx, r14
	xor	edx, edx
	mov	r8d, 1
	call	qword ptr [rax + 80]
	mov	qword ptr [rsp + 80], 0
	mov	rax, qword ptr [rsi + 104]
	mov	qword ptr [rsp + 88], rax
	lea	rcx, [rsp + 80]
	mov	rdx, rdi
	call	"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z"
	test	al, al
	je	.LBB34_11
# %bb.10:
	mov	eax, dword ptr [rsi + 120524]
	add	dword ptr [rsp + 84], eax
	add	dword ptr [rsp + 92], eax
	mov	rax, qword ptr [r14]
	lea	r8, [rsp + 80]
	mov	rcx, r14
	mov	edx, 1
	call	qword ptr [rax + 360]
	mov	rax, qword ptr [r14]
	mov	dword ptr [rsp + 32], 0
	mov	rcx, r14
	mov	edx, 4
	mov	r8d, 1
	xor	r9d, r9d
	call	qword ptr [rax + 168]
	inc	dword ptr [rsi + 99312]
.LBB34_11:
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 112], xmm0
	mov	qword ptr [rsp + 128], 0
	mov	rax, qword ptr [r14]
	lea	r9, [rsp + 112]
	mov	rcx, r14
	xor	edx, edx
	mov	r8d, 3
	call	qword ptr [rax + 64]
	mov	eax, r15d
	jmp	.LBB34_12
.LBB34_2:
	test	rdi, rdi
	jne	.LBB34_12
# %bb.3:
	vmovsd	xmm0, qword ptr [r14]           # xmm0 = mem[0],zero
	vmovsd	qword ptr [rsp + 56], xmm0
	vmovss	xmm0, dword ptr [r14 + 8]       # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 64], xmm0
	mov	dword ptr [rsp + 68], 1065353216
	lea	rdx, [rsp + 56]
	mov	rcx, rsi
	call	"?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z"
	mov	al, 1
.LBB34_12:
	.seh_startepilogue
	add	rsp, 160
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z",unique,36
	.p2align	4                               # -- Begin function ??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z
"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z": # @"??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
.seh_proc "??R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 112
	.seh_stackalloc 112
	.seh_endprologue
	mov	esi, r8d
	mov	edi, edx
	mov	rbx, rcx
	mov	byte ptr [rsp + 63], r9b
	vmovups	xmm0, xmmword ptr [rcx]
	mov	r14, qword ptr [rcx]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	lea	rax, [rsp + 63]
	mov	qword ptr [rsp + 80], rax
	mov	rax, qword ptr [rcx + 16]
	mov	qword ptr [rsp + 88], rax
	lea	r15, [rsp + 96]
	lea	rdx, [rsp + 64]
	mov	rcx, r15
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [rbx + 24]
	mov	rax, qword ptr [rbx + 32]
	mov	qword ptr [rsp + 48], r15
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 14
	mov	rcx, r14
	mov	edx, edi
	mov	r8d, esi
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	nop
	.seh_startepilogue
	add	rsp, 112
	pop	rbx
	pop	rdi
	pop	rsi
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z",unique,37
	.p2align	4                               # -- Begin function ??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z
"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z": # @"??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
.seh_proc "??R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 112
	.seh_stackalloc 112
	.seh_endprologue
	mov	esi, r8d
	mov	edi, edx
	mov	rbx, rcx
	vmovups	ymm0, ymmword ptr [rcx]
	mov	r14, qword ptr [rcx]
	vmovups	ymmword ptr [rsp + 80], ymm0
	lea	r15, [rsp + 64]
	lea	rdx, [rsp + 80]
	mov	rcx, r15
	vzeroupper
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z"
	mov	r9, qword ptr [rbx + 32]
	mov	rax, qword ptr [rbx + 40]
	mov	qword ptr [rsp + 48], r15
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 10
	mov	rcx, r14
	mov	edx, edi
	mov	r8d, esi
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	nop
	.seh_startepilogue
	add	rsp, 112
	pop	rbx
	pop	rdi
	pop	rsi
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z",unique,38
	.p2align	4                               # -- Begin function ??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z
"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z": # @"??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
.seh_proc "??R<lambda_4>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@W4RendererInlineMediaLayer@2@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 112
	.seh_stackalloc 112
	.seh_endprologue
	mov	r8, qword ptr [rcx + 8]
	mov	al, 1
	cmp	byte ptr [r8], 1
	jne	.LBB37_4
# %bb.1:
	mov	r8, qword ptr [rcx + 16]
	movzx	r8d, byte ptr [r8]
	or	r8b, dl
	je	.LBB37_3
# %bb.2:
	mov	r8, qword ptr [rcx + 24]
	cmp	byte ptr [r8], 1
	jne	.LBB37_4
.LBB37_3:
	mov	rax, qword ptr [rcx]
	mov	r15, qword ptr [rcx + 32]
	movzx	ecx, dl
	mov	esi, dword ptr [rax + 4*rcx + 68824]
	mov	edi, dword ptr [rax + 4*rcx + 68864]
	mov	byte ptr [rsp + 63], 0
	vmovups	xmm0, xmmword ptr [r15]
	mov	rbx, qword ptr [r15]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	lea	rax, [rsp + 63]
	mov	qword ptr [rsp + 80], rax
	mov	rax, qword ptr [r15 + 16]
	mov	qword ptr [rsp + 88], rax
	lea	r14, [rsp + 96]
	lea	rdx, [rsp + 64]
	mov	rcx, r14
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [r15 + 24]
	mov	rax, qword ptr [r15 + 32]
	mov	qword ptr [rsp + 48], r14
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 14
	mov	rcx, rbx
	mov	edx, esi
	mov	r8d, edi
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	nop
.LBB37_4:
	.seh_startepilogue
	add	rsp, 112
	pop	rbx
	pop	rdi
	pop	rsi
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z",unique,39
	.p2align	4                               # -- Begin function ??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z
"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z": # @"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
.seh_proc "??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbx
	.seh_pushreg rbx
	add	rsp, -128
	.seh_stackalloc 128
	.seh_endprologue
	mov	esi, r8d
	mov	edi, edx
	mov	rbx, rcx
	mov	byte ptr [rsp + 63], r9b
	vmovups	xmm0, xmmword ptr [rcx]
	mov	r14, qword ptr [rcx]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	mov	rax, qword ptr [rcx + 16]
	mov	qword ptr [rsp + 80], rax
	lea	rax, [rsp + 63]
	mov	qword ptr [rsp + 88], rax
	mov	rax, qword ptr [rcx + 24]
	mov	qword ptr [rsp + 96], rax
	lea	r15, [rsp + 112]
	lea	rdx, [rsp + 64]
	mov	rcx, r15
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [rbx + 32]
	mov	rax, qword ptr [rbx + 40]
	mov	qword ptr [rsp + 48], r15
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 8
	mov	rcx, r14
	mov	edx, edi
	mov	r8d, esi
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	nop
	.seh_startepilogue
	add	rsp, 128
	pop	rbx
	pop	rdi
	pop	rsi
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z",unique,40
	.p2align	4                               # -- Begin function ??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z
"??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z": # @"??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
.seh_proc "??R<lambda_6>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 136
	.seh_stackalloc 136
	.seh_endprologue
	mov	ebp, edx
	mov	rdi, rcx
	mov	rax, qword ptr [rcx + 8]
	cmp	byte ptr [rax], 0
	je	.LBB39_11
# %bb.1:
	mov	byte ptr [rsp + 63], r9b        # 1-byte Spill
	mov	rax, qword ptr [rdi + 24]
	mov	r15d, dword ptr [rax]
	mov	rax, qword ptr [rdi]
	mov	ecx, dword ptr [rax + 120356]
	cmp	ecx, r15d
	cmovb	r15d, ecx
	cmp	ebp, r15d
	cmova	r15d, ebp
	mov	eax, dword ptr [rax + 120360]
	mov	rcx, qword ptr [rdi + 48]
	mov	ebx, dword ptr [rcx]
	cmp	eax, ebx
	cmovb	ebx, eax
	cmp	r8d, ebx
	mov	r12d, ebp
	mov	dword ptr [rsp + 116], ebx      # 4-byte Spill
	cmova	ebx, r8d
	mov	r14, qword ptr [rdi + 16]
	vmovups	ymm0, ymmword ptr [r14]
	mov	r13, qword ptr [r14]
	vmovups	ymmword ptr [rsp + 64], ymm0
	mov	ebp, r8d
	lea	rsi, [rsp + 120]
	lea	rdx, [rsp + 64]
	mov	rcx, rsi
	vzeroupper
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z"
	mov	r9, qword ptr [r14 + 32]
	mov	rax, qword ptr [r14 + 40]
	mov	qword ptr [rsp + 48], rsi
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 10
	mov	rcx, r13
	mov	edx, r12d
	mov	r8d, r15d
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
# %bb.2:
	mov	r14, qword ptr [rdi + 40]
	movzx	eax, byte ptr [rsp + 63]        # 1-byte Folded Reload
	mov	byte ptr [rsp + 62], al
	vmovups	xmm0, xmmword ptr [r14]
	mov	r12, qword ptr [r14]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	mov	rax, qword ptr [r14 + 16]
	mov	qword ptr [rsp + 80], rax
	lea	rax, [rsp + 62]
	mov	qword ptr [rsp + 88], rax
	mov	rax, qword ptr [r14 + 24]
	mov	qword ptr [rsp + 96], rax
	lea	rdx, [rsp + 64]
	mov	rcx, rsi
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [r14 + 32]
	mov	rax, qword ptr [r14 + 40]
	mov	qword ptr [rsp + 48], rsi
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 8
	mov	rcx, r12
	mov	edx, ebp
	mov	r8d, ebx
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
# %bb.3:
	mov	dword ptr [rsp + 112], ebx      # 4-byte Spill
	mov	rax, qword ptr [rdi + 32]
	mov	rcx, qword ptr [rax + 8]
	cmp	byte ptr [rcx], 1
	jne	.LBB39_6
# %bb.4:
	mov	rcx, qword ptr [rax + 24]
	cmp	byte ptr [rcx], 1
	jne	.LBB39_6
# %bb.5:
	mov	rcx, qword ptr [rax]
	mov	r14, qword ptr [rax + 32]
	mov	ebx, dword ptr [rcx + 68860]
	mov	r13d, dword ptr [rcx + 68900]
	mov	byte ptr [rsp + 62], 0
	vmovups	xmm0, xmmword ptr [r14]
	mov	r12, qword ptr [r14]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	lea	rax, [rsp + 62]
	mov	qword ptr [rsp + 80], rax
	mov	rax, qword ptr [r14 + 16]
	mov	qword ptr [rsp + 88], rax
	lea	rdx, [rsp + 64]
	lea	rsi, [rsp + 120]
	mov	rcx, rsi
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [r14 + 24]
	mov	rax, qword ptr [r14 + 32]
	mov	qword ptr [rsp + 48], rsi
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 14
	mov	rcx, r12
	mov	edx, ebx
	mov	r8d, r13d
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
.LBB39_6:
	mov	r14, qword ptr [rdi + 16]
	mov	rax, qword ptr [rdi + 24]
	mov	r12d, dword ptr [rax]
	vmovups	ymm0, ymmword ptr [r14]
	mov	r13, qword ptr [r14]
	vmovups	ymmword ptr [rsp + 64], ymm0
	lea	rdx, [rsp + 64]
	lea	rsi, [rsp + 120]
	mov	rcx, rsi
	vzeroupper
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z"
	mov	r9, qword ptr [r14 + 32]
	mov	rax, qword ptr [r14 + 40]
	mov	qword ptr [rsp + 48], rsi
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 10
	mov	rcx, r13
	mov	edx, r15d
	mov	r8d, r12d
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
# %bb.7:
	mov	rax, qword ptr [rdi + 32]
	mov	rcx, qword ptr [rax + 8]
	cmp	byte ptr [rcx], 1
	jne	.LBB39_10
# %bb.8:
	mov	rcx, qword ptr [rax + 24]
	cmp	byte ptr [rcx], 1
	jne	.LBB39_10
# %bb.9:
	mov	rcx, qword ptr [rax]
	mov	r14, qword ptr [rax + 32]
	mov	r15d, dword ptr [rcx + 68852]
	mov	r12d, dword ptr [rcx + 68892]
	mov	byte ptr [rsp + 62], 0
	vmovups	xmm0, xmmword ptr [r14]
	mov	r13, qword ptr [r14]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	lea	rax, [rsp + 62]
	mov	qword ptr [rsp + 80], rax
	mov	rax, qword ptr [r14 + 16]
	mov	qword ptr [rsp + 88], rax
	lea	rdx, [rsp + 64]
	lea	rsi, [rsp + 120]
	mov	rcx, rsi
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [r14 + 24]
	mov	rax, qword ptr [r14 + 32]
	mov	qword ptr [rsp + 48], rsi
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 14
	mov	rcx, r13
	mov	edx, r15d
	mov	r8d, r12d
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
.LBB39_10:
	cmp	ebp, dword ptr [rsp + 116]      # 4-byte Folded Reload
	setae	al
	movzx	r9d, byte ptr [rsp + 63]        # 1-byte Folded Reload
	and	r9b, al
	mov	rcx, qword ptr [rdi + 40]
	mov	rax, qword ptr [rdi + 48]
	mov	r8d, dword ptr [rax]
	mov	edx, dword ptr [rsp + 112]      # 4-byte Reload
	call	"??R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z"
	jmp	.LBB39_16
.LBB39_11:
	mov	esi, r8d
	mov	r13d, r9d
	mov	rbx, qword ptr [rdi + 16]
	mov	rax, qword ptr [rdi + 24]
	mov	r15d, dword ptr [rax]
	vmovups	ymm0, ymmword ptr [rbx]
	mov	r12, qword ptr [rbx]
	vmovups	ymmword ptr [rsp + 64], ymm0
	lea	r14, [rsp + 120]
	lea	rdx, [rsp + 64]
	mov	rcx, r14
	vzeroupper
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z"
	mov	r9, qword ptr [rbx + 32]
	mov	rax, qword ptr [rbx + 40]
	mov	qword ptr [rsp + 48], r14
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 10
	mov	rcx, r12
	mov	edx, ebp
	mov	r8d, r15d
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
# %bb.12:
	mov	rax, qword ptr [rdi + 32]
	mov	rcx, qword ptr [rax + 8]
	cmp	byte ptr [rcx], 1
	jne	.LBB39_15
# %bb.13:
	mov	rcx, qword ptr [rax + 24]
	cmp	byte ptr [rcx], 1
	jne	.LBB39_15
# %bb.14:
	mov	rcx, qword ptr [rax]
	mov	rbx, qword ptr [rax + 32]
	mov	ebp, dword ptr [rcx + 68852]
	mov	r15d, dword ptr [rcx + 68892]
	mov	byte ptr [rsp + 62], 0
	vmovups	xmm0, xmmword ptr [rbx]
	mov	r12, qword ptr [rbx]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	lea	rax, [rsp + 62]
	mov	qword ptr [rsp + 80], rax
	mov	rax, qword ptr [rbx + 16]
	mov	qword ptr [rsp + 88], rax
	lea	rdx, [rsp + 64]
	mov	rcx, r14
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [rbx + 24]
	mov	rax, qword ptr [rbx + 32]
	mov	qword ptr [rsp + 48], r14
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 14
	mov	rcx, r12
	mov	edx, ebp
	mov	r8d, r15d
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
	test	al, al
	je	.LBB39_23
.LBB39_15:
	mov	r15, qword ptr [rdi + 40]
	mov	rax, qword ptr [rdi + 48]
	mov	edi, dword ptr [rax]
	mov	byte ptr [rsp + 62], r13b
	vmovups	xmm0, xmmword ptr [r15]
	mov	rbx, qword ptr [r15]
	vmovaps	xmmword ptr [rsp + 64], xmm0
	mov	rax, qword ptr [r15 + 16]
	mov	qword ptr [rsp + 80], rax
	lea	rax, [rsp + 62]
	mov	qword ptr [rsp + 88], rax
	mov	rax, qword ptr [r15 + 24]
	mov	qword ptr [rsp + 96], rax
	lea	rdx, [rsp + 64]
	mov	rcx, r14
	call	"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
	mov	r9, qword ptr [r15 + 32]
	mov	rax, qword ptr [r15 + 40]
	mov	qword ptr [rsp + 48], r14
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], 8
	mov	rcx, rbx
	mov	edx, esi
	mov	r8d, edi
	call	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
.LBB39_16:
                                        # kill: def $al killed $al def $eax
	jmp	.LBB39_24
.LBB39_23:
	xor	eax, eax
.LBB39_24:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 136
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z",unique,41
	.p2align	4                               # -- Begin function ?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z
"?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z": # @"?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z"
.seh_proc "?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 152
	.seh_stackalloc 152
	vmovaps	xmmword ptr [rsp + 128], xmm9   # 16-byte Spill
	.seh_savexmm xmm9, 128
	vmovaps	xmmword ptr [rsp + 112], xmm8   # 16-byte Spill
	.seh_savexmm xmm8, 112
	vmovaps	xmmword ptr [rsp + 96], xmm7    # 16-byte Spill
	.seh_savexmm xmm7, 96
	vmovaps	xmmword ptr [rsp + 80], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 80
	.seh_endprologue
	vmovaps	xmm6, xmm3
	vmovaps	xmm7, xmm2
	mov	rdi, rdx
	mov	rsi, rcx
	vmovss	xmm8, dword ptr [rsp + 216]     # xmm8 = mem[0],zero,zero,zero
	vmovss	xmm9, dword ptr [rsp + 208]     # xmm9 = mem[0],zero,zero,zero
	vucomiss	xmm2, dword ptr [rdx]
	jne	.LBB40_4
	jp	.LBB40_4
# %bb.1:
	vucomiss	xmm6, dword ptr [rsi + 120316]
	jne	.LBB40_4
	jp	.LBB40_4
# %bb.2:
	vucomiss	xmm9, dword ptr [rsi + 120320]
	jne	.LBB40_4
	jp	.LBB40_4
# %bb.3:
	mov	al, 1
	vucomiss	xmm8, dword ptr [rsi + 120324]
	jne	.LBB40_4
	jnp	.LBB40_7
.LBB40_4:
	mov	eax, dword ptr [rsi + 112]
	mov	r9d, dword ptr [rsi + 108]
	mov	r8d, dword ptr [rsi + 104]
	mov	rdx, qword ptr [rsi + 8]
	lea	rcx, [rsi + 22856]
	vmovss	dword ptr [rsp + 64], xmm8
	vmovss	dword ptr [rsp + 56], xmm9
	vmovss	dword ptr [rsp + 48], xmm6
	vmovss	dword ptr [rsp + 40], xmm7
	mov	dword ptr [rsp + 32], eax
	call	"?d3d11_draw_pipeline_update_layer@maicord@@YA_NAEBUD3D11DrawPipelineOwner@1@PEAUID3D11DeviceContext@@IIIMMMM@Z"
	test	al, al
	je	.LBB40_5
# %bb.6:
	vmovss	dword ptr [rdi], xmm7
	vmovss	dword ptr [rsi + 70432], xmm7
	vmovss	dword ptr [rsi + 120316], xmm6
	vmovss	dword ptr [rsi + 120320], xmm9
	vmovss	dword ptr [rsi + 120324], xmm8
	mov	al, 1
	jmp	.LBB40_7
.LBB40_5:
	xor	eax, eax
.LBB40_7:
                                        # kill: def $al killed $al killed $eax
	vmovaps	xmm6, xmmword ptr [rsp + 80]    # 16-byte Reload
	vmovaps	xmm7, xmmword ptr [rsp + 96]    # 16-byte Reload
	vmovaps	xmm8, xmmword ptr [rsp + 112]   # 16-byte Reload
	vmovaps	xmm9, xmmword ptr [rsp + 128]   # 16-byte Reload
	.seh_startepilogue
	add	rsp, 152
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__real@c1000000                 # -- Begin function ?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z
	.section	.rdata,"dr",discard,__real@c1000000
	.p2align	2, 0x0
__real@c1000000:
	.long	0xc1000000                      # float -8
	.section	.text,"xr",one_only,"?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z",unique,42
	.p2align	4
"?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z": # @"?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z"
.seh_proc "?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 296
	.seh_stackalloc 296
	vmovaps	xmmword ptr [rsp + 272], xmm6   # 16-byte Spill
	.seh_savexmm xmm6, 272
	.seh_endprologue
	cmp	r8d, r9d
	ja	.LBB41_1
# %bb.3:
	mov	rdi, rcx
	cmp	r9d, dword ptr [rcx + 82736]
	jbe	.LBB41_4
.LBB41_1:
	xor	eax, eax
.LBB41_2:
                                        # kill: def $al killed $al killed $eax
	vmovaps	xmm6, xmmword ptr [rsp + 272]   # 16-byte Reload
	.seh_startepilogue
	add	rsp, 296
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
.LBB41_4:
	mov	al, 1
	cmp	r8d, r9d
	je	.LBB41_2
# %bb.5:
	mov	ecx, dword ptr [rdi + 99220]
	test	ecx, ecx
	mov	qword ptr [rsp + 192], rdx      # 8-byte Spill
	je	.LBB41_6
# %bb.7:
	mov	r11d, dword ptr [rdi + 99216]
	mov	eax, dword ptr [rdi + 99228]
	mov	qword ptr [rsp + 96], rdi
	mov	qword ptr [rsp + 104], rdx
	mov	byte ptr [rsp + 112], 1
	mov	r10d, 4294967295
	mov	qword ptr [rsp + 116], r10
	mov	dword ptr [rsp + 124], 0
	cmp	eax, r11d
	je	.LBB41_8
# %bb.9:
	mov	dword ptr [rsp + 128], r11d
	add	ecx, r11d
	jmp	.LBB41_10
.LBB41_6:
	mov	qword ptr [rsp + 96], rdi
	mov	qword ptr [rsp + 104], rdx
	mov	byte ptr [rsp + 112], 1
	mov	eax, 4294967295
	mov	qword ptr [rsp + 116], rax
	mov	dword ptr [rsp + 124], 0
	mov	eax, dword ptr [rdi + 99228]
.LBB41_8:
	mov	dword ptr [rsp + 128], 0
	xor	ecx, ecx
.LBB41_10:
	mov	rdx, qword ptr [rdi + 8]
	mov	qword ptr [rsp + 200], rdx      # 8-byte Spill
	mov	dword ptr [rsp + 132], ecx
	mov	dword ptr [rsp + 136], eax
	movzx	r13d, byte ptr [rdi + 70210]
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 160], xmm0
	mov	qword ptr [rsp + 176], 0
	cmp	r8d, r9d
	jae	.LBB41_32
# %bb.11:
	mov	r10, qword ptr [rsp + 408]
	vmovss	xmm2, dword ptr [rsp + 400]     # xmm2 = mem[0],zero,zero,zero
	lea	r15, [rdi + 79624]
	lea	r12, [rsp + 224]
	lea	rdx, [rsp + 208]
	vmovss	xmm6, dword ptr [rip + __real@c1000000] # xmm6 = [-8.0E+0,0.0E+0,0.0E+0,0.0E+0]
	mov	dword ptr [rsp + 88], r9d       # 4-byte Spill
	jmp	.LBB41_12
.LBB41_28:                              #   in Loop: Header=BB41_12 Depth=1
	mov	r8, r9
	shl	r8, 6
	add	r8, qword ptr [rdi + 75520]
	mov	qword ptr [rsp + 64], r12
	vmovss	xmm0, dword ptr [rsp + 400]     # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 48], xmm0
	lea	rax, [rsp + 256]
	mov	qword ptr [rsp + 40], rax
	mov	dword ptr [rsp + 32], esi
	mov	dword ptr [rsp + 56], -1
	mov	rcx, rdi
	mov	rdx, qword ptr [rsp + 408]
	mov	rbx, r9
	mov	r9d, 64
	call	"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z"
	mov	r9, rbx
	add	esi, r9d
.LBB41_29:                              #   in Loop: Header=BB41_12 Depth=1
	mov	dword ptr [rsp + 32], esi
	lea	rcx, [rsp + 96]
	lea	rdx, [rsp + 160]
	mov	r8, r12
                                        # kill: def $r9d killed $r9d killed $r9
	call	"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
.LBB41_30:                              #   in Loop: Header=BB41_12 Depth=1
	mov	r9d, dword ptr [rsp + 88]       # 4-byte Reload
	mov	r8d, dword ptr [rsp + 92]       # 4-byte Reload
	mov	r10, qword ptr [rsp + 408]
	vmovss	xmm2, dword ptr [rsp + 400]     # xmm2 = mem[0],zero,zero,zero
	lea	rdx, [rsp + 208]
.LBB41_31:                              #   in Loop: Header=BB41_12 Depth=1
	inc	r8d
	cmp	r8d, r9d
	je	.LBB41_32
.LBB41_12:                              # =>This Loop Header: Depth=1
                                        #     Child Loop BB41_35 Depth 2
	mov	eax, r8d
	lea	r11, [rax + 2*rax]
	mov	ecx, dword ptr [r15 + 4*r11]
	mov	eax, dword ptr [rdi + 82728]
	cmp	eax, ecx
	jb	.LBB41_1
# %bb.13:                               #   in Loop: Header=BB41_12 Depth=1
	lea	rbx, [r15 + 4*r11]
	mov	r11d, dword ptr [rbx + 4]
	sub	eax, ecx
	cmp	r11d, eax
	ja	.LBB41_1
# %bb.14:                               #   in Loop: Header=BB41_12 Depth=1
	mov	ebp, dword ptr [rdi + 22992]
	xor	eax, eax
	sub	ebp, ecx
	jb	.LBB41_2
# %bb.15:                               #   in Loop: Header=BB41_12 Depth=1
	cmp	r11d, ebp
	ja	.LBB41_2
# %bb.16:                               #   in Loop: Header=BB41_12 Depth=1
	test	r11d, r11d
	je	.LBB41_31
# %bb.17:                               #   in Loop: Header=BB41_12 Depth=1
	movzx	r14d, word ptr [rbx + 8]
	cmp	dword ptr [rdi + 82732], r14d
	jbe	.LBB41_31
# %bb.18:                               #   in Loop: Header=BB41_12 Depth=1
	mov	dword ptr [rsp + 92], r8d       # 4-byte Spill
	mov	rax, qword ptr [rdi + 75520]
	shl	rcx, 6
	vxorps	xmm0, xmm0, xmm0
	vucomiss	xmm0, dword ptr [rax + rcx]
	jb	.LBB41_26
# %bb.19:                               #   in Loop: Header=BB41_12 Depth=1
	add	rax, rcx
	vucomiss	xmm0, dword ptr [rax + 4]
	jb	.LBB41_26
# %bb.20:                               #   in Loop: Header=BB41_12 Depth=1
	mov	ecx, dword ptr [rdi + 104]
	vcvtsi2ss	xmm0, xmm5, rcx
	vmulss	xmm1, xmm2, dword ptr [rax + 8]
	vucomiss	xmm1, xmm0
	jb	.LBB41_26
# %bb.21:                               #   in Loop: Header=BB41_12 Depth=1
	mov	ecx, dword ptr [rdi + 108]
	vcvtsi2ss	xmm0, xmm5, rcx
	vmulss	xmm1, xmm2, dword ptr [rax + 12]
	vucomiss	xmm1, xmm0
	jb	.LBB41_26
# %bb.22:                               #   in Loop: Header=BB41_12 Depth=1
	vmovss	xmm0, dword ptr [rax + 48]      # xmm0 = mem[0],zero,zero,zero
	vxorps	xmm1, xmm1, xmm1
	vucomiss	xmm0, xmm1
	jne	.LBB41_26
	jp	.LBB41_26
# %bb.23:                               #   in Loop: Header=BB41_12 Depth=1
	vmovss	xmm0, dword ptr [rax + 52]      # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, xmm1
	jne	.LBB41_26
	jp	.LBB41_26
# %bb.24:                               #   in Loop: Header=BB41_12 Depth=1
	vmovss	xmm0, dword ptr [rax + 56]      # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, xmm6
	jne	.LBB41_25
	jnp	.LBB41_26
.LBB41_25:                              #   in Loop: Header=BB41_12 Depth=1
	vmovss	xmm2, dword ptr [rdi + 70432]   # xmm2 = mem[0],zero,zero,zero
	vmovups	xmm0, xmmword ptr [rax + 16]
	vmovaps	xmmword ptr [rsp + 240], xmm0
	mov	rcx, rdi
	mov	rbp, r15
	mov	r15, rdx
	lea	rdx, [rsp + 240]
	mov	rsi, r12
	mov	r12, r10
	call	"?d3d11_renderer_caption_note_scrim@maicord@@YAXPEAUD3D11Renderer@1@UColor@1@M@Z"
	mov	rdx, r15
	mov	r15, rbp
	vmovss	xmm2, dword ptr [rsp + 400]     # xmm2 = mem[0],zero,zero,zero
	mov	r10, r12
	mov	r12, rsi
.LBB41_26:                              #   in Loop: Header=BB41_12 Depth=1
	shl	r14d, 4
	lea	rax, [rdi + 75528]
	add	r14, rax
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 224], xmm0
	vmovups	xmm0, xmmword ptr [r14]
	vmovaps	xmmword ptr [rsp + 208], xmm0
	lea	rax, [rsp + 256]
	mov	qword ptr [rsp + 40], rax
	mov	qword ptr [rsp + 32], r12
	mov	rcx, rdi
	mov	r9, r10
	call	"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z"
	test	al, al
	je	.LBB41_30
# %bb.27:                               #   in Loop: Header=BB41_12 Depth=1
	mov	r9d, dword ptr [rbx]
	mov	esi, dword ptr [rbx + 4]
	test	r13b, r13b
	je	.LBB41_28
# %bb.33:                               #   in Loop: Header=BB41_12 Depth=1
	add	esi, r9d
	cmp	r9d, esi
	jae	.LBB41_29
# %bb.34:                               #   in Loop: Header=BB41_12 Depth=1
	mov	byte ptr [rsp + 83], r13b       # 1-byte Spill
	mov	dword ptr [rsp + 84], esi       # 4-byte Spill
	mov	r13d, esi
	mov	r15, r9
	shl	r15, 6
	mov	rbp, r9
	jmp	.LBB41_35
	.p2align	4
.LBB41_41:                              #   in Loop: Header=BB41_35 Depth=2
                                        # kill: def $r9d killed $r9d killed $r9 def $r9
.LBB41_42:                              #   in Loop: Header=BB41_35 Depth=2
	inc	rbp
	add	r15, 64
	cmp	rbp, r13
	jae	.LBB41_43
.LBB41_35:                              #   Parent Loop BB41_12 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	mov	rbx, qword ptr [rdi + 75520]
	vmovss	xmm0, dword ptr [rbx + r15 + 56] # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, xmm6
	jne	.LBB41_42
	jp	.LBB41_42
# %bb.36:                               #   in Loop: Header=BB41_35 Depth=2
	vmovss	xmm0, dword ptr [rbx + r15 + 52] # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, xmm6
	jne	.LBB41_37
	jnp	.LBB41_38
.LBB41_37:                              #   in Loop: Header=BB41_35 Depth=2
	vxorps	xmm1, xmm1, xmm1
	vucomiss	xmm0, xmm1
	jne	.LBB41_42
	jp	.LBB41_42
.LBB41_38:                              #   in Loop: Header=BB41_35 Depth=2
	mov	r8, r12
	lea	rax, [rbp + 1]
	mov	dword ptr [rsp + 32], eax
	lea	rsi, [rsp + 96]
	mov	rcx, rsi
	lea	r14, [rsp + 160]
	mov	rdx, r14
                                        # kill: def $r9d killed $r9d killed $r9
	call	"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
	mov	rcx, rsi
	mov	rdx, r14
	call	"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 160], xmm0
	mov	qword ptr [rsp + 176], 0
	vmovss	xmm0, dword ptr [rbx + r15]     # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 144], xmm0
	vmovss	xmm0, dword ptr [rbx + r15 + 4] # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 148], xmm0
	vmovss	xmm0, dword ptr [rbx + r15 + 8] # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 152], xmm0
	vmovss	xmm0, dword ptr [rbx + r15 + 12] # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 156], xmm0
	lea	rcx, [rdi + 69368]
	mov	rdx, qword ptr [rsp + 200]      # 8-byte Reload
	lea	r8, [rsp + 144]
	mov	r9d, ebp
	call	"?d3d11_background_fx_draw@maicord@@YA_NPEAUD3D11BackgroundFxOwner@1@PEAUID3D11DeviceContext@@UUiRect@1@I@Z"
	lea	r9, [rbp + 1]
	test	al, al
	je	.LBB41_41
# %bb.39:                               #   in Loop: Header=BB41_35 Depth=2
	inc	dword ptr [rdi + 99312]
	cmp	byte ptr [rdi + 70210], 2
	mov	rcx, qword ptr [rsp + 192]      # 8-byte Reload
	mov	byte ptr [rcx + 16], 0
	movzx	eax, byte ptr [rcx + 19]
	mov	byte ptr [rcx + 21], al
	mov	byte ptr [rcx + 22], 0
	jne	.LBB41_41
# %bb.40:                               #   in Loop: Header=BB41_35 Depth=2
	mov	byte ptr [rcx + 18], 0
	jmp	.LBB41_41
.LBB41_43:                              #   in Loop: Header=BB41_12 Depth=1
	movzx	r13d, byte ptr [rsp + 83]       # 1-byte Folded Reload
	lea	r15, [rdi + 79624]
	mov	esi, dword ptr [rsp + 84]       # 4-byte Reload
	jmp	.LBB41_29
.LBB41_32:
	lea	rcx, [rsp + 96]
	lea	rdx, [rsp + 160]
	call	"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
	mov	al, 1
	jmp	.LBB41_2
	.seh_endproc
                                        # -- End function
	.def	"?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z",unique,43
	.p2align	4                               # -- Begin function ?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z
"?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z": # @"?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z"
.seh_proc "?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 296
	.seh_stackalloc 296
	.seh_endprologue
	mov	r14d, dword ptr [rsp + 400]
	mov	rdi, qword ptr [rcx + 88]
	lea	r10, [rdi + 132]
	lea	rax, [rcx + 23616]
	test	rdi, rdi
	cmove	r10, rax
	je	.LBB42_2
# %bb.1:
	mov	r11d, dword ptr [rdi + 136]
	xor	eax, eax
	cmp	r14d, r9d
	jae	.LBB42_3
	jmp	.LBB42_9
.LBB42_2:
	mov	r11d, 1
	xor	eax, eax
	cmp	r14d, r9d
	jb	.LBB42_9
.LBB42_3:
	cmp	r14d, r11d
	ja	.LBB42_9
# %bb.4:
	mov	al, 1
	cmp	r14d, r9d
	je	.LBB42_9
# %bb.5:
	cmp	byte ptr [rcx + 23628], 1
	jne	.LBB42_9
# %bb.6:
	mov	ebx, dword ptr [r10]
	test	ebx, ebx
	je	.LBB42_9
# %bb.7:
	mov	eax, dword ptr [rcx + 22984]
	cmp	ebx, eax
	jbe	.LBB42_10
.LBB42_8:
	xor	eax, eax
.LBB42_9:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 296
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
.LBB42_10:
	movzx	r10d, byte ptr [rsp + 408]
	test	rdi, rdi
	mov	qword ptr [rsp + 80], rcx       # 8-byte Spill
	je	.LBB42_14
# %bb.11:
	cmp	qword ptr [rdi], 0
	je	.LBB42_8
# %bb.12:
	cmp	qword ptr [rdi + 8], 0
	je	.LBB42_8
# %bb.13:
	cmp	qword ptr [rcx + 120432], rdi
	je	.LBB42_22
.LBB42_14:
	test	r10b, r10b
	je	.LBB42_46
# %bb.15:
	mov	qword ptr [rsp + 128], r8       # 8-byte Spill
	mov	qword ptr [rsp + 120], rdx      # 8-byte Spill
	mov	dword ptr [rsp + 92], r9d       # 4-byte Spill
	xor	esi, esi
.LBB42_16:
	mov	edx, ebx
	sub	edx, esi
	mov	r8d, dword ptr [rcx + 22988]
	sub	eax, r8d
	xor	r9d, r9d
	cmp	edx, eax
	cmova	esi, r9d
	cmovbe	r9d, r8d
	test	r8d, r8d
	cmove	esi, r8d
	mov	dword ptr [rsp + 96], r9d       # 4-byte Spill
	cmp	r9d, 1
	mov	r9d, 5
	sbb	r9d, 0
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 224], xmm0
	mov	rax, qword ptr [rcx + 8]
	mov	rdx, qword ptr [rcx + 22960]
	mov	r10, qword ptr [rax]
	lea	rcx, [rsp + 224]
	mov	qword ptr [rsp + 40], rcx
	mov	dword ptr [rsp + 32], 0
	mov	rcx, rax
	xor	r8d, r8d
	call	qword ptr [r10 + 112]
	test	eax, eax
	js	.LBB42_21
# %bb.17:
	mov	r13d, dword ptr [rsp + 96]      # 4-byte Reload
	test	r13d, r13d
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
	jne	.LBB42_19
# %bb.18:
	inc	dword ptr [rdx + 120508]
.LBB42_19:
	mov	eax, ebx
	sub	eax, esi
	mov	ecx, r13d
	lea	rcx, [rcx + 2*rcx]
	shl	rcx, 4
	add	rcx, qword ptr [rsp + 224]
	test	rdi, rdi
	mov	r9, qword ptr [rsp + 128]       # 8-byte Reload
	je	.LBB42_30
# %bb.20:
	mov	ebp, esi
	mov	edx, esi
	lea	rdx, [rdx + 2*rdx]
	shl	rdx, 4
	add	rdx, qword ptr [rdi]
	mov	r15d, eax
	mov	eax, eax
	shl	rax, 4
	lea	rsi, [rax + 2*rax]
	mov	r8, rsi
	call	memcpy
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
	jmp	.LBB42_37
.LBB42_21:
	lea	rdx, [rip + "??_C@_06IAAOMEKN@render?$AA@"]
	lea	r8, [rip + "??_C@_0CB@JFPKHMFH@glyph_instance_buffer_map_failed@"]
	mov	cl, 3
	call	"?log_write@maicord@@YAXW4LogLevel@1@PEBD1@Z"
	jmp	.LBB42_8
.LBB42_22:
	mov	esi, dword ptr [rcx + 120424]
	mov	r11b, 1
	cmp	esi, dword ptr [rdi + 148]
	jne	.LBB42_24
# %bb.23:
	cmp	dword ptr [rcx + 120428], ebx
	setne	r11b
.LBB42_24:
	test	r10b, r10b
	je	.LBB42_46
# %bb.25:
	test	r11b, r11b
	je	.LBB42_46
# %bb.26:
	mov	r10d, dword ptr [rcx + 120440]
	xor	esi, esi
	cmp	r10d, ebx
	mov	qword ptr [rsp + 120], rdx      # 8-byte Spill
	mov	dword ptr [rsp + 92], r9d       # 4-byte Spill
	mov	qword ptr [rsp + 128], r8       # 8-byte Spill
	ja	.LBB42_16
# %bb.27:
	cmp	r10d, dword ptr [rcx + 120488]
	ja	.LBB42_16
# %bb.28:
	mov	edx, dword ptr [rcx + 120504]
	cmp	edx, dword ptr [rcx + 120508]
	jne	.LBB42_16
# %bb.29:
	mov	edx, dword ptr [rcx + 120500]
	xor	r8d, r8d
	cmp	edx, dword ptr [rcx + 120444]
	cmovb	r10d, r8d
	mov	esi, r10d
	jmp	.LBB42_16
.LBB42_30:
	cmp	byte ptr [rdx + 120537], 1
	jne	.LBB42_62
# %bb.31:
	vxorps	xmm0, xmm0, xmm0
.LBB42_32:
	sub	r9, -128
	xor	r10d, r10d
	mov	r11, rbx
	jmp	.LBB42_34
.LBB42_33:                              #   in Loop: Header=BB42_34 Depth=1
	add	r10, 48
	dec	r11
	je	.LBB42_36
.LBB42_34:                              # =>This Inner Loop Header: Depth=1
	mov	r15, qword ptr [rdx + 23600]
	vmovups	ymm1, ymmword ptr [r15 + r10]
	vmovups	ymm2, ymmword ptr [r15 + r10 + 16]
	vmovups	ymmword ptr [rcx + r10], ymm1
	vmovups	ymmword ptr [rcx + r10 + 16], ymm2
	vaddps	xmm1, xmm0, xmmword ptr [r15 + r10]
	vmovups	xmmword ptr [rcx + r10], xmm1
	cmp	byte ptr [rdx + 120537], 0
	jne	.LBB42_33
# %bb.35:                               #   in Loop: Header=BB42_34 Depth=1
	vmovups	xmm1, xmmword ptr [r9]
	vmovups	xmmword ptr [rcx + r10 + 32], xmm1
	jmp	.LBB42_33
.LBB42_36:
	mov	ebp, esi
	mov	r15d, eax
	mov	eax, eax
	shl	rax, 4
	lea	rsi, [rax + 2*rax]
.LBB42_37:
	mov	rcx, qword ptr [rdx + 8]
	mov	rdx, qword ptr [rdx + 22960]
	mov	rax, qword ptr [rcx]
	xor	r12d, r12d
	xor	r8d, r8d
	vzeroupper
	call	qword ptr [rax + 120]
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	add	r15d, r13d
	mov	dword ptr [rcx + 22988], r15d
	inc	dword ptr [rcx + 99344]
	add	qword ptr [rcx + 99360], rsi
	test	ebp, ebp
	je	.LBB42_39
# %bb.38:
	mov	dword ptr [rcx + 120488], ebp
	mov	dword ptr [rcx + 120496], r13d
	mov	r9d, dword ptr [rsp + 92]       # 4-byte Reload
	mov	rdx, qword ptr [rsp + 120]      # 8-byte Reload
	jmp	.LBB42_42
.LBB42_39:
	mov	dword ptr [rcx + 120488], ebx
	mov	dword ptr [rcx + 120496], r13d
	mov	dword ptr [rcx + 120492], r13d
	test	rdi, rdi
	mov	r9d, dword ptr [rsp + 92]       # 4-byte Reload
	mov	rdx, qword ptr [rsp + 120]      # 8-byte Reload
	je	.LBB42_41
# %bb.40:
	mov	r12d, dword ptr [rdi + 148]
.LBB42_41:
	mov	dword ptr [rcx + 120500], r12d
	mov	eax, dword ptr [rcx + 120508]
	mov	dword ptr [rcx + 120504], eax
.LBB42_42:
	test	rdi, rdi
	je	.LBB42_44
# %bb.43:
	mov	rax, qword ptr [rcx + 88]
	mov	qword ptr [rcx + 120432], rax
	mov	eax, dword ptr [rax + 148]
	mov	dword ptr [rcx + 120424], eax
	mov	dword ptr [rcx + 120428], ebx
	jmp	.LBB42_45
.LBB42_44:
	mov	qword ptr [rcx + 120432], 0
.LBB42_45:
	mov	eax, dword ptr [rcx + 22984]
.LBB42_46:
	mov	esi, dword ptr [rcx + 120488]
	cmp	esi, ebx
	cmovae	esi, ebx
	mov	r8d, dword ptr [rcx + 120492]
	add	r8d, esi
	cmp	r8d, eax
	ja	.LBB42_8
# %bb.47:
	mov	eax, ebx
	sub	eax, esi
	add	eax, dword ptr [rcx + 120496]
	cmp	eax, dword ptr [rcx + 22988]
	ja	.LBB42_8
# %bb.48:
	vmovq	xmm0, qword ptr [rcx + 120492]  # xmm0 = mem[0],zero
	test	rdi, rdi
	jne	.LBB42_51
# %bb.50:
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB42_65
.LBB42_51:
	xor	eax, eax
.LBB42_52:
	mov	r8d, dword ptr [rcx + 112]
	mov	qword ptr [rsp + 224], rcx
	mov	qword ptr [rsp + 232], rdx
	mov	byte ptr [rsp + 240], 2
	mov	dword ptr [rsp + 244], esi
	vmovq	xmm0, xmm0                      # xmm0 = xmm0[0],zero
	vmovdqu	xmmword ptr [rsp + 248], xmm0
	mov	dword ptr [rsp + 264], 0
	vpxor	xmm0, xmm0, xmm0
	vmovdqa	xmmword ptr [rsp + 192], xmm0
	mov	qword ptr [rsp + 208], 0
	mov	ebp, r14d
	sub	ebp, r9d
	jbe	.LBB42_74
# %bb.53:
	vcvtsi2ss	xmm0, xmm5, r8
	mov	r10, qword ptr [rsp + 416]
	vdivss	xmm2, xmm0, dword ptr [rip + __real@42c00000]
	test	rdi, rdi
	vmovss	dword ptr [rsp + 96], xmm2      # 4-byte Spill
	je	.LBB42_67
# %bb.54:
	mov	rdi, qword ptr [rcx + 88]
	mov	rdx, qword ptr [rdi + 8]
	mov	r11d, r9d
	lea	r8, [r11 + 2*r11]
	mov	r9, qword ptr [rdx + 8*r8 + 16]
	mov	qword ptr [rsp + 160], r9
	vmovups	xmm0, xmmword ptr [rdx + 8*r8]
	vmovaps	xmmword ptr [rsp + 144], xmm0
	mov	ebp, dword ptr [rsp + 164]
	test	ebp, ebp
	je	.LBB42_8
# %bb.55:
	mov	r9, r10
	dec	r14d
	sub	r14, r11
	inc	r14
	lea	r13, [8*r8 + 24]
	vxorps	xmm0, xmm0, xmm0
	lea	r11, [rsp + 280]
	lea	r10, [rsp + 176]
	lea	r8, [rsp + 144]
	mov	qword ptr [rsp + 272], rbx      # 8-byte Spill
.LBB42_56:                              # =>This Inner Loop Header: Depth=1
	mov	r12d, dword ptr [rsp + 160]
	mov	esi, ebx
	sub	esi, r12d
	jb	.LBB42_8
# %bb.57:                               #   in Loop: Header=BB42_56 Depth=1
	cmp	ebp, esi
	ja	.LBB42_8
# %bb.58:                               #   in Loop: Header=BB42_56 Depth=1
	vmovaps	xmmword ptr [rsp + 176], xmm0
	mov	rsi, r11
	mov	qword ptr [rsp + 40], r11
	mov	r15, r10
	mov	qword ptr [rsp + 32], r10
	mov	rdx, r8
	call	"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z"
	test	al, al
	je	.LBB42_60
# %bb.59:                               #   in Loop: Header=BB42_56 Depth=1
	mov	qword ptr [rsp + 128], r14      # 8-byte Spill
	lea	r8, [r12 + 2*r12]
	shl	r8, 4
	add	r8, qword ptr [rdi]
	mov	qword ptr [rsp + 64], r15
	vmovss	xmm0, dword ptr [rsp + 96]      # 4-byte Reload
                                        # xmm0 = mem[0],zero,zero,zero
	vmovss	dword ptr [rsp + 48], xmm0
	mov	qword ptr [rsp + 40], rsi
	mov	dword ptr [rsp + 32], ebp
	mov	dword ptr [rsp + 56], 32
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	rdx, qword ptr [rsp + 416]
	mov	r9d, 48
	call	"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z"
	add	ebp, r12d
	mov	dword ptr [rsp + 32], ebp
	lea	rcx, [rsp + 224]
	lea	rdx, [rsp + 192]
	mov	r8, r15
	mov	r9d, r12d
	mov	r14, qword ptr [rsp + 128]      # 8-byte Reload
	mov	rbx, qword ptr [rsp + 272]      # 8-byte Reload
	call	"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
.LBB42_60:                              #   in Loop: Header=BB42_56 Depth=1
	dec	r14
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r9, qword ptr [rsp + 416]
	vmovss	xmm2, dword ptr [rsp + 96]      # 4-byte Reload
                                        # xmm2 = mem[0],zero,zero,zero
	je	.LBB42_74
# %bb.61:                               #   in Loop: Header=BB42_56 Depth=1
	mov	rdi, qword ptr [rcx + 88]
	mov	rdx, qword ptr [rdi + 8]
	mov	r8, qword ptr [rdx + r13 + 16]
	mov	qword ptr [rsp + 160], r8
	vmovups	xmm0, xmmword ptr [rdx + r13]
	add	r13, 24
	vmovaps	xmmword ptr [rsp + 144], xmm0
	mov	ebp, dword ptr [rsp + 164]
	test	ebp, ebp
	mov	r10, r15
	lea	r8, [rsp + 144]
	vxorps	xmm0, xmm0, xmm0
	mov	r11, rsi
	jne	.LBB42_56
	jmp	.LBB42_8
.LBB42_62:
	mov	ebp, eax
	mov	r15, rcx
	mov	r12, r9
	mov	rcx, qword ptr [rdx + 61032]
	call	"?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z"
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
	test	rax, rax
	setne	cl
	movzx	eax, byte ptr [rdx + 120537]
	or	cl, al
	je	.LBB42_75
# %bb.63:
	vxorps	xmm0, xmm0, xmm0
	test	al, al
	je	.LBB42_76
# %bb.64:
	mov	r9, r12
	mov	rcx, r15
	mov	eax, ebp
	mov	r13d, dword ptr [rsp + 96]      # 4-byte Reload
	jmp	.LBB42_32
.LBB42_65:
	vmovdqa	xmmword ptr [rsp + 96], xmm0    # 16-byte Spill
	mov	r15, rdx
	mov	ebp, r9d
	mov	rcx, qword ptr [rcx + 61032]
	call	"?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z"
	test	rax, rax
	jne	.LBB42_78
# %bb.66:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	cmp	byte ptr [rcx + 120537], 0
	mov	r9d, ebp
	mov	rdx, r15
	vmovdqa	xmm0, xmmword ptr [rsp + 96]    # 16-byte Reload
	je	.LBB42_8
	jmp	.LBB42_52
.LBB42_67:
	add	rax, 112
	vmovd	xmm0, ebx
	vpshufd	xmm0, xmm0, 81                  # xmm0 = xmm0[1,0,1,1]
	lea	r8, [rsp + 144]
	vxorps	xmm1, xmm1, xmm1
	lea	r14, [rsp + 280]
	lea	rdx, [rsp + 176]
	lea	rdi, [rsp + 224]
	lea	rsi, [rsp + 192]
	mov	r9, r10
	mov	r12, rax
	mov	r15, r8
	vmovdqa	xmmword ptr [rsp + 128], xmm0   # 16-byte Spill
	jmp	.LBB42_69
.LBB42_68:                              #   in Loop: Header=BB42_69 Depth=1
	dec	ebp
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r9, qword ptr [rsp + 416]
	vmovss	xmm2, dword ptr [rsp + 96]      # 4-byte Reload
                                        # xmm2 = mem[0],zero,zero,zero
	mov	rax, r12
	mov	r8, r15
	lea	rdx, [rsp + 176]
	vmovdqa	xmm0, xmmword ptr [rsp + 128]   # 16-byte Reload
	vxorps	xmm1, xmm1, xmm1
	je	.LBB42_74
.LBB42_69:                              # =>This Inner Loop Header: Depth=1
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB42_71
# %bb.70:                               #   in Loop: Header=BB42_69 Depth=1
	mov	rdx, qword ptr [rcx + 52008]
	mov	rcx, r8
	xor	r8d, r8d
	call	"?login_scene_rectangle@maicord@@YA?AUUiRect@1@AEBULoginScene@1@W4LoginElement@1@@Z"
	vxorps	xmm1, xmm1, xmm1
	vmovdqa	xmm0, xmmword ptr [rsp + 128]   # 16-byte Reload
	lea	rdx, [rsp + 176]
	mov	r8, r15
	vmovss	xmm2, dword ptr [rsp + 96]      # 4-byte Reload
                                        # xmm2 = mem[0],zero,zero,zero
	mov	r9, qword ptr [rsp + 416]
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	jmp	.LBB42_72
.LBB42_71:                              #   in Loop: Header=BB42_69 Depth=1
	vmovups	xmm3, xmmword ptr [rax]
	vmovaps	xmmword ptr [rsp + 144], xmm3
.LBB42_72:                              #   in Loop: Header=BB42_69 Depth=1
	vmovq	qword ptr [rsp + 160], xmm0
	vmovaps	xmmword ptr [rsp + 176], xmm1
	mov	qword ptr [rsp + 40], r14
	mov	qword ptr [rsp + 32], rdx
	mov	rdx, r8
	call	"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z"
	test	al, al
	je	.LBB42_68
# %bb.73:                               #   in Loop: Header=BB42_69 Depth=1
	mov	dword ptr [rsp + 32], ebx
	mov	rcx, rdi
	mov	rdx, rsi
	lea	r8, [rsp + 176]
	xor	r9d, r9d
	call	"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
	jmp	.LBB42_68
.LBB42_74:
	lea	rcx, [rsp + 224]
	lea	rdx, [rsp + 192]
	call	"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
	mov	al, 1
	jmp	.LBB42_9
.LBB42_75:
	mov	rcx, qword ptr [rdx + 8]
	mov	rdx, qword ptr [rdx + 22960]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	jmp	.LBB42_8
.LBB42_76:
	mov	rdx, qword ptr [rdx + 61032]
	lea	rcx, [rsp + 192]
	mov	r8b, 7
	call	"?ui_scene_content_rectangle@maicord@@YA?AUUiRect@1@AEBUUiScene@1@W4UiSemantic@1@@Z"
	vxorps	xmm0, xmm0, xmm0
	mov	rdx, qword ptr [rsp + 80]       # 8-byte Reload
	test	byte ptr [rdx + 120537], 1
	mov	r9, r12
	mov	rcx, r15
	mov	eax, ebp
	mov	r13d, dword ptr [rsp + 96]      # 4-byte Reload
	jne	.LBB42_32
# %bb.77:
	vmovddup	xmm0, qword ptr [rsp + 192]     # xmm0 = mem[0,0]
	jmp	.LBB42_32
.LBB42_78:
	mov	rcx, qword ptr [rsp + 80]       # 8-byte Reload
	mov	r9d, ebp
	mov	rdx, r15
	vmovdqa	xmm0, xmmword ptr [rsp + 96]    # 16-byte Reload
	jmp	.LBB42_52
	.seh_endproc
                                        # -- End function
	.def	"??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z",unique,44
	.p2align	4                               # -- Begin function ??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z
"??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z": # @"??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z"
.seh_proc "??R<lambda_7>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@2@AEBUTheme@2@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@2@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 56
	.seh_stackalloc 56
	.seh_endprologue
	mov	ebx, edx
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx]
	mov	rax, qword ptr [rsi + 8]
	vmovss	xmm0, dword ptr [rax]           # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, dword ptr [rip + __real@3f800000]
	jne	.LBB43_2
	jp	.LBB43_2
# %bb.1:
	vmovss	xmm0, dword ptr [rcx + 120316]  # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, dword ptr [rip + __real@3f800000]
	jne	.LBB43_2
	jnp	.LBB43_3
.LBB43_2:
	mov	eax, dword ptr [rcx + 112]
	mov	r9d, dword ptr [rcx + 108]
	mov	r8d, dword ptr [rcx + 104]
	mov	rdx, qword ptr [rcx + 8]
	add	rcx, 22856
	mov	dword ptr [rsp + 32], eax
	mov	dword ptr [rsp + 40], 1065353216
	call	"?d3d11_draw_pipeline_update_opacity@maicord@@YA_NAEBUD3D11DrawPipelineOwner@1@PEAUID3D11DeviceContext@@IIIM@Z"
	and	bl, al
	mov	rcx, qword ptr [rsi]
.LBB43_3:
	mov	dword ptr [rcx + 120316], 1065353216
	mov	rcx, qword ptr [rsi]
	mov	rax, qword ptr [rsi + 16]
	movzx	edx, byte ptr [rax + 24]
	call	"?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z"
	test	bl, bl
	je	.LBB43_5
# %bb.4:
	mov	rax, qword ptr [rsi]
	mov	ecx, dword ptr [rax + 99324]
	add	ecx, dword ptr [rax + 99320]
	mov	edx, -1
	cmovae	edx, ecx
	mov	dword ptr [rax + 99324], edx
.LBB43_5:
	mov	rcx, qword ptr [rsi]
	mov	rdx, qword ptr [rcx + 8]
	add	rcx, 69368
	call	"?d3d11_background_fx_end_frame@maicord@@YAXPEAUD3D11BackgroundFxOwner@1@PEAUID3D11DeviceContext@@@Z"
	mov	rcx, qword ptr [rsi]
	mov	rax, qword ptr [rsi + 24]
	mov	r8, qword ptr [rax]
	mov	rdx, qword ptr [rcx + 8]
	add	rcx, 69280
	call	"?d3d11_gpu_timer_end@maicord@@YAXPEAUD3D11GpuTimerOwner@1@PEAUID3D11DeviceContext@@PEAUD3D11GpuTimerFrame@1@@Z"
	mov	eax, ebx
	.seh_startepilogue
	add	rsp, 56
	pop	rbx
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?com_release_slot@maicord@@YAXPEAPEAX@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?com_release_slot@maicord@@YAXPEAPEAX@Z",unique,45
	.globl	"?com_release_slot@maicord@@YAXPEAPEAX@Z" # -- Begin function ?com_release_slot@maicord@@YAXPEAPEAX@Z
	.p2align	4
"?com_release_slot@maicord@@YAXPEAPEAX@Z": # @"?com_release_slot@maicord@@YAXPEAPEAX@Z"
.seh_proc "?com_release_slot@maicord@@YAXPEAPEAX@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB44_2
# %bb.1:
	mov	rax, qword ptr [rcx]
	call	qword ptr [rax + 16]
	mov	qword ptr [rsi], 0
.LBB44_2:
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,46
	.globl	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 66288]
	test	rcx, rcx
	je	.LBB45_4
# %bb.1:
	call	"?ui_thread_browser_scene_current@maicord@@YA_NAEBUUiThreadBrowserScene@1@@Z"
	test	al, al
	je	.LBB45_4
# %bb.2:
	cmp	byte ptr [rsi + 120537], 0
	je	.LBB45_7
.LBB45_4:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB45_7:
	mov	rax, qword ptr [rsi + 63968]
	test	rax, rax
	je	.LBB45_12
# %bb.8:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB45_12
# %bb.9:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB45_4
# %bb.10:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB45_4
# %bb.11:
	cmp	byte ptr [rsi + 120537], 0
	jne	.LBB45_15
.LBB45_12:
	mov	rcx, qword ptr [rsi + 63976]
	test	rcx, rcx
	je	.LBB45_15
# %bb.13:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB45_4
.LBB45_15:
	mov	rax, qword ptr [rsi + 64024]
	test	rax, rax
	je	.LBB45_17
# %bb.16:
	movzx	eax, byte ptr [rax + 1101]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
.LBB45_17:
	mov	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,47
	.globl	"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_message_modal_active@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	mov	rdx, qword ptr [rcx + 66312]
	test	rdx, rdx
	je	.LBB46_2
# %bb.1:
	mov	al, 1
	cmp	byte ptr [rdx + 2040], 0
	jne	.LBB46_9
.LBB46_2:
	mov	rdx, qword ptr [rcx + 66328]
	test	rdx, rdx
	je	.LBB46_4
# %bb.3:
	mov	al, 1
	cmp	byte ptr [rdx + 2042], 0
	jne	.LBB46_9
.LBB46_4:
	mov	rdx, qword ptr [rcx + 66344]
	test	rdx, rdx
	je	.LBB46_6
# %bb.5:
	mov	al, 1
	cmp	byte ptr [rdx + 1009], 0
	jne	.LBB46_9
.LBB46_6:
	mov	rax, qword ptr [rcx + 66360]
	test	rax, rax
	je	.LBB46_7
# %bb.8:
	movzx	eax, byte ptr [rax + 1392]
.LBB46_9:
                                        # kill: def $al killed $al killed $eax
	ret
.LBB46_7:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	ret
                                        # -- End function
	.def	"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,48
	.globl	"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_inline_scene_covered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	sil, 1
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB47_2
# %bb.1:
	mov	rdi, rcx
	call	"?d3d11_renderer_thread_browser_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	je	.LBB47_3
.LBB47_2:
	mov	eax, esi
	.seh_startepilogue
	add	rsp, 40
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
.LBB47_3:
	mov	rax, qword ptr [rdi + 63968]
	test	rax, rax
	je	.LBB47_8
# %bb.4:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB47_8
# %bb.5:
	cmp	byte ptr [rdi + 120537], 0
	jne	.LBB47_2
# %bb.6:
	mov	rcx, qword ptr [rdi + 63976]
	test	rcx, rcx
	je	.LBB47_2
# %bb.7:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB47_2
.LBB47_8:
	cmp	byte ptr [rdi + 120537], 0
	jne	.LBB47_11
# %bb.9:
	mov	rcx, qword ptr [rdi + 63976]
	test	rcx, rcx
	je	.LBB47_11
# %bb.10:
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	jne	.LBB47_2
.LBB47_11:
	mov	rax, qword ptr [rdi + 64024]
	test	rax, rax
	je	.LBB47_13
# %bb.12:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB47_2
.LBB47_13:
	mov	rax, qword ptr [rdi + 64032]
	test	rax, rax
	je	.LBB47_15
# %bb.14:
	cmp	byte ptr [rax + 1101], 0
	jne	.LBB47_2
.LBB47_15:
	mov	rax, qword ptr [rdi + 22736]
	test	rax, rax
	je	.LBB47_17
# %bb.16:
	cmp	byte ptr [rax + 8583], 0
	jne	.LBB47_2
.LBB47_17:
	mov	rax, qword ptr [rdi + 63904]
	test	rax, rax
	je	.LBB47_19
# %bb.18:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB47_2
.LBB47_19:
	mov	rcx, rdi
	call	"?d3d11_renderer_media_picker_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB47_2
# %bb.20:
	mov	rcx, rdi
	call	"?d3d11_renderer_quick_switcher_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB47_2
# %bb.21:
	mov	rcx, rdi
	call	"?d3d11_renderer_download_visible@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
	test	al, al
	jne	.LBB47_2
# %bb.22:
	mov	rax, qword ptr [rdi + 61104]
	test	rax, rax
	je	.LBB47_24
# %bb.23:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB47_2
.LBB47_24:
	mov	rax, qword ptr [rdi + 63896]
	test	rax, rax
	je	.LBB47_26
# %bb.25:
	cmp	byte ptr [rax + 3909], 0
	jne	.LBB47_2
.LBB47_26:
	mov	rax, qword ptr [rdi + 63880]
	test	rax, rax
	je	.LBB47_29
# %bb.27:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB47_29
# %bb.28:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB47_2
.LBB47_29:
	mov	rax, qword ptr [rdi + 63888]
	test	rax, rax
	je	.LBB47_33
# %bb.30:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB47_33
# %bb.31:
	movzx	esi, byte ptr [rax + 1428]
	jmp	.LBB47_2
.LBB47_33:
	xor	esi, esi
	jmp	.LBB47_2
	.seh_endproc
                                        # -- End function
	.def	"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z",unique,49
	.globl	"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z" # -- Begin function ?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z
	.p2align	4
"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z": # @"?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
.seh_proc "?d3d11_renderer_modal_uncovered@maicord@@YA_NAEBUD3D11Renderer@1@@Z"
# %bb.0:
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	cmp	byte ptr [rcx + 120537], 0
	je	.LBB48_2
.LBB48_1:
	xor	eax, eax
.LBB48_30:
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
.LBB48_2:
	mov	rax, qword ptr [rcx + 63968]
	test	rax, rax
	je	.LBB48_7
# %bb.3:
	cmp	byte ptr [rax + 636], 1
	jne	.LBB48_7
# %bb.4:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB48_1
# %bb.5:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	test	al, al
	je	.LBB48_1
# %bb.6:
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	cmp	byte ptr [rcx + 120537], 0
	jne	.LBB48_10
.LBB48_7:
	mov	rax, qword ptr [rcx + 63976]
	test	rax, rax
	je	.LBB48_10
# %bb.8:
	mov	qword ptr [rsp + 32], rcx       # 8-byte Spill
	mov	rcx, rax
	call	"?ui_theme_editor_current@maicord@@YA_NAEBUUiThemeEditorScene@1@@Z"
	mov	rcx, qword ptr [rsp + 32]       # 8-byte Reload
	test	al, al
	jne	.LBB48_1
.LBB48_10:
	mov	rax, qword ptr [rcx + 22736]
	test	rax, rax
	je	.LBB48_13
# %bb.11:
	cmp	byte ptr [rax + 8583], 0
	jne	.LBB48_1
.LBB48_13:
	mov	rax, qword ptr [rcx + 63904]
	test	rax, rax
	je	.LBB48_16
# %bb.14:
	cmp	byte ptr [rax + 2638], 0
	jne	.LBB48_1
.LBB48_16:
	mov	rax, qword ptr [rcx + 61104]
	test	rax, rax
	je	.LBB48_19
# %bb.17:
	cmp	byte ptr [rax + 398], 0
	jne	.LBB48_1
.LBB48_19:
	mov	rax, qword ptr [rcx + 63896]
	test	rax, rax
	je	.LBB48_22
# %bb.20:
	cmp	byte ptr [rax + 3909], 0
	jne	.LBB48_1
.LBB48_22:
	mov	rax, qword ptr [rcx + 63880]
	test	rax, rax
	je	.LBB48_27
# %bb.23:
	mov	rax, qword ptr [rax]
	test	rax, rax
	je	.LBB48_27
# %bb.24:
	cmp	byte ptr [rax + 704], 0
	jne	.LBB48_1
.LBB48_27:
	mov	rcx, qword ptr [rcx + 63888]
	mov	al, 1
	test	rcx, rcx
	je	.LBB48_30
# %bb.28:
	mov	rcx, qword ptr [rcx]
	test	rcx, rcx
	je	.LBB48_30
# %bb.29:
	movzx	eax, byte ptr [rcx + 1428]
	xor	al, 1
                                        # kill: def $al killed $al killed $eax
	.seh_startepilogue
	add	rsp, 40
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z",unique,50
	.p2align	4                               # -- Begin function ?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z
"?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z": # @"?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z"
.seh_proc "?clear_target@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@QEBM@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 32
	.seh_stackalloc 32
	.seh_endprologue
	mov	r8, rdx
	mov	rsi, rcx
	mov	rcx, qword ptr [rcx + 8]
	mov	rdx, qword ptr [rsi + 56]
	mov	rax, qword ptr [rcx]
	call	qword ptr [rax + 400]
	mov	rcx, rsi
	.seh_startepilogue
	add	rsp, 32
	pop	rsi
	.seh_endepilogue
	jmp	"?d3d11_renderer_caption_invalidate@maicord@@YAXPEAUD3D11Renderer@1@@Z" # TAILCALL
	.seh_endproc
                                        # -- End function
	.def	"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z",unique,51
	.p2align	4                               # -- Begin function ?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z
"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z": # @"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z"
# %bb.0:
	mov	eax, dword ptr [rcx]
	test	rdx, rdx
	je	.LBB50_1
# %bb.2:
	mov	r8d, dword ptr [rdx]
	cmp	eax, r8d
	cmovle	eax, r8d
	mov	dword ptr [rcx], eax
	vmovd	xmm0, dword ptr [rcx + 4]       # xmm0 = mem[0],zero,zero,zero
	vmovd	xmm1, dword ptr [rdx + 4]       # xmm1 = mem[0],zero,zero,zero
	vpmaxsd	xmm0, xmm0, xmm1
	vmovd	dword ptr [rcx + 4], xmm0
	mov	r8d, dword ptr [rdx + 8]
	mov	r9d, dword ptr [rcx + 8]
	cmp	r9d, r8d
	cmovl	r8d, r9d
	mov	dword ptr [rcx + 8], r8d
	vmovd	xmm0, dword ptr [rcx + 12]      # xmm0 = mem[0],zero,zero,zero
	vmovd	xmm1, dword ptr [rdx + 12]      # xmm1 = mem[0],zero,zero,zero
	vpminsd	xmm0, xmm0, xmm1
	vmovd	dword ptr [rcx + 12], xmm0
	cmp	eax, r8d
	jge	.LBB50_4
.LBB50_5:
	mov	eax, dword ptr [rcx + 4]
	cmp	eax, dword ptr [rcx + 12]
	setl	al
                                        # kill: def $al killed $al killed $eax
	ret
.LBB50_1:
	mov	r8d, dword ptr [rcx + 8]
	cmp	eax, r8d
	jl	.LBB50_5
.LBB50_4:
	xor	eax, eax
                                        # kill: def $al killed $al killed $eax
	ret
                                        # -- End function
	.def	"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__xmm@0000000000000000400000003f800000 # -- Begin function ?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z
	.section	.rdata,"dr",discard,__xmm@0000000000000000400000003f800000
	.p2align	4, 0x0
__xmm@0000000000000000400000003f800000:
	.long	0x3f800000                      # float 1
	.long	0x40000000                      # float 2
	.zero	4
	.zero	4
	.section	.text,"xr",one_only,"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z",unique,52
	.p2align	4
"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z": # @"?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
.seh_proc "?draw_opacity_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@IIPEBULayerBatchRange@12@IAEAMUBatchDraw@12@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 152
	.seh_stackalloc 152
	vmovdqa	xmmword ptr [rsp + 128], xmm11  # 16-byte Spill
	.seh_savexmm xmm11, 128
	vmovaps	xmmword ptr [rsp + 112], xmm10  # 16-byte Spill
	.seh_savexmm xmm10, 112
	vmovaps	xmmword ptr [rsp + 96], xmm9    # 16-byte Spill
	.seh_savexmm xmm9, 96
	vmovaps	xmmword ptr [rsp + 80], xmm8    # 16-byte Spill
	.seh_savexmm xmm8, 80
	vmovaps	xmmword ptr [rsp + 64], xmm7    # 16-byte Spill
	.seh_savexmm xmm7, 64
	vmovaps	xmmword ptr [rsp + 48], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 48
	.seh_endprologue
	mov	ebp, edx
	mov	rax, qword ptr [rsp + 272]
	cmp	edx, r8d
	jne	.LBB51_1
# %bb.16:
	mov	rcx, qword ptr [rax]
	mov	edx, ebp
	mov	r8d, ebp
	vmovaps	xmm6, xmmword ptr [rsp + 48]    # 16-byte Reload
	vmovaps	xmm7, xmmword ptr [rsp + 64]    # 16-byte Reload
	vmovaps	xmm8, xmmword ptr [rsp + 80]    # 16-byte Reload
	vmovaps	xmm9, xmmword ptr [rsp + 96]    # 16-byte Reload
	vmovaps	xmm10, xmmword ptr [rsp + 112]  # 16-byte Reload
	vmovaps	xmm11, xmmword ptr [rsp + 128]  # 16-byte Reload
	.seh_startepilogue
	add	rsp, 152
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	rex64 jmp	qword ptr [rax + 8]     # TAILCALL
.LBB51_1:
	mov	rdi, r9
	mov	esi, r8d
	mov	rbx, rcx
	mov	r13, rax
	mov	r14, qword ptr [rsp + 264]
	mov	eax, dword ptr [rsp + 256]
	shl	rax, 3
	lea	r12, [rax + 2*rax]
	vmovss	xmm6, dword ptr [rip + __real@3f800000] # xmm6 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vxorps	xmm7, xmm7, xmm7
	vxorps	xmm8, xmm8, xmm8
	vmovsd	xmm9, qword ptr [rip + __xmm@0000000000000000400000003f800000] # xmm9 = [1.0E+0,2.0E+0,0.0E+0,0.0E+0]
	vbroadcastss	xmm10, dword ptr [rip + __real@3f800000] # xmm10 = [1.0E+0,1.0E+0,1.0E+0,1.0E+0]
	vpcmpeqd	xmm11, xmm11, xmm11
	.p2align	4
.LBB51_2:                               # =>This Loop Header: Depth=1
                                        #     Child Loop BB51_4 Depth 2
	mov	r15d, ebp
	cmp	ebp, esi
	jae	.LBB51_15
# %bb.3:                                #   in Loop: Header=BB51_2 Depth=1
	vxorps	xmm0, xmm0, xmm0
	xor	eax, eax
	vmovaps	xmm3, xmm6
	vmovaps	xmm2, xmm6
	mov	ebp, esi
	jmp	.LBB51_4
.LBB51_9:                               #   in Loop: Header=BB51_4 Depth=2
	vmulss	xmm2, xmm2, xmm1
	cmp	ebp, ecx
	cmovae	ebp, ecx
	.p2align	4
.LBB51_12:                              #   in Loop: Header=BB51_4 Depth=2
	add	rax, 24
	cmp	r12, rax
	je	.LBB51_13
.LBB51_4:                               #   Parent Loop BB51_2 Depth=1
                                        # =>  This Inner Loop Header: Depth=2
	vmovsd	xmm1, qword ptr [rdi + rax + 8] # xmm1 = mem[0],zero
	vcmpnless	xmm4, xmm7, xmm1
	vcmpnltps	xmm5, xmm8, xmm1
	vblendps	xmm4, xmm4, xmm5, 2             # xmm4 = xmm4[0],xmm5[1],xmm4[2,3]
	vcmpnleps	xmm5, xmm1, xmm9
	vorps	xmm4, xmm4, xmm5
	vblendvps	xmm1, xmm1, xmm10, xmm4
	vcmpeqps	xmm4, xmm10, xmm1
	vshufps	xmm4, xmm4, xmm4, 80            # xmm4 = xmm4[0,0,1,1]
	vtestpd	xmm4, xmm11
	jb	.LBB51_12
# %bb.5:                                #   in Loop: Header=BB51_4 Depth=2
	mov	edx, dword ptr [rdi + rax]
	mov	ecx, dword ptr [rdi + rax + 4]
	cmp	edx, ecx
	jae	.LBB51_12
# %bb.6:                                #   in Loop: Header=BB51_4 Depth=2
	cmp	r15d, edx
	setb	r8b
	cmp	r15d, ecx
	setae	r9b
	or	r9b, r8b
	jne	.LBB51_10
# %bb.7:                                #   in Loop: Header=BB51_4 Depth=2
	vcmpeqps	xmm4, xmm10, xmm1
	vpextrb	edx, xmm4, 4
	test	dl, 1
	jne	.LBB51_9
# %bb.8:                                #   in Loop: Header=BB51_4 Depth=2
	vmovsd	xmm0, qword ptr [rdi + rax + 16] # xmm0 = mem[0],zero
	vmovshdup	xmm3, xmm1              # xmm3 = xmm1[1,1,3,3]
	jmp	.LBB51_9
.LBB51_10:                              #   in Loop: Header=BB51_4 Depth=2
	cmp	r15d, edx
	jae	.LBB51_12
# %bb.11:                               #   in Loop: Header=BB51_4 Depth=2
	cmp	ebp, edx
	cmovae	ebp, edx
	jmp	.LBB51_12
	.p2align	4
.LBB51_13:                              #   in Loop: Header=BB51_2 Depth=1
	vextractps	dword ptr [rsp + 40], xmm0, 1
	vmovss	dword ptr [rsp + 32], xmm0
	mov	rcx, rbx
	mov	rdx, r14
	call	"?bind_layer@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@AEAMMMMM@Z"
	test	al, al
	je	.LBB51_15
# %bb.14:                               #   in Loop: Header=BB51_2 Depth=1
	mov	rcx, qword ptr [r13]
	mov	edx, r15d
	mov	r8d, ebp
	call	qword ptr [r13 + 8]
	test	al, al
	jne	.LBB51_2
.LBB51_15:
	cmp	r15d, esi
	setae	al
	vmovaps	xmm6, xmmword ptr [rsp + 48]    # 16-byte Reload
	vmovaps	xmm7, xmmword ptr [rsp + 64]    # 16-byte Reload
	vmovaps	xmm8, xmmword ptr [rsp + 80]    # 16-byte Reload
	vmovaps	xmm9, xmmword ptr [rsp + 96]    # 16-byte Reload
	vmovaps	xmm10, xmmword ptr [rsp + 112]  # 16-byte Reload
	vmovaps	xmm11, xmmword ptr [rsp + 128]  # 16-byte Reload
	.seh_startepilogue
	add	rsp, 152
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z",unique,53
	.p2align	4                               # -- Begin function ??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z
"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z": # @"??$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
# %bb.0:
	mov	qword ptr [rcx], rdx
	lea	rax, [rip + "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"]
	mov	qword ptr [rcx + 8], rax
	ret
                                        # -- End function
	.def	"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z",unique,54
	.p2align	4                               # -- Begin function ?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z
"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z": # @"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"
.seh_proc "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 48
	.seh_stackalloc 48
	.seh_endprologue
	mov	r9d, r8d
	mov	r8d, edx
	mov	rsi, rcx
	mov	rax, qword ptr [rcx + 24]
	mov	rax, qword ptr [rax]
	mov	rcx, qword ptr [rcx + 16]
	movzx	r10d, byte ptr [rcx]
	mov	rcx, qword ptr [rsi]
	mov	rdx, qword ptr [rsi + 8]
	mov	qword ptr [rsp + 40], rax
	mov	byte ptr [rsp + 32], r10b
	call	"?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z"
	mov	rcx, qword ptr [rsi + 16]
	mov	byte ptr [rcx], 0
	.seh_startepilogue
	add	rsp, 48
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z",unique,55
	.p2align	4                               # -- Begin function ?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z
"?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z": # @"?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z"
.seh_proc "?draw_gpu_media@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@II_NPEBUtagRECT@@@Z"
# %bb.0:
	push	r15
	.seh_pushreg r15
	push	r14
	.seh_pushreg r14
	push	r13
	.seh_pushreg r13
	push	r12
	.seh_pushreg r12
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 216
	.seh_stackalloc 216
	vmovaps	xmmword ptr [rsp + 192], xmm6   # 16-byte Spill
	.seh_savexmm xmm6, 192
	.seh_endprologue
	mov	rdi, rcx
	mov	ecx, dword ptr [rcx + 96]
	mov	al, 1
	test	ecx, ecx
	je	.LBB54_25
# %bb.1:
	cmp	qword ptr [rdi + 52192], 0
	je	.LBB54_24
# %bb.2:
	cmp	qword ptr [rdi + 52280], 0
	je	.LBB54_24
# %bb.3:
	cmp	qword ptr [rdi + 22928], 0
	je	.LBB54_24
# %bb.4:
	mov	esi, r9d
	xor	eax, eax
	cmp	r9d, r8d
	jb	.LBB54_25
# %bb.5:
	cmp	ecx, 4096
	ja	.LBB54_25
# %bb.6:
	mov	r9, qword ptr [rdi + 22968]
	test	r9, r9
	je	.LBB54_25
# %bb.7:
	mov	eax, dword ptr [rdi + 100]
	cmp	esi, eax
	jbe	.LBB54_8
.LBB54_24:
	xor	eax, eax
.LBB54_25:
                                        # kill: def $al killed $al killed $eax
	vmovaps	xmm6, xmmword ptr [rsp + 192]   # 16-byte Reload
	.seh_startepilogue
	add	rsp, 216
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r12
	pop	r13
	pop	r14
	pop	r15
	.seh_endepilogue
	ret
.LBB54_8:
	cmp	byte ptr [rsp + 320], 0
	je	.LBB54_17
# %bb.9:
	test	rax, rax
	je	.LBB54_15
# %bb.10:
	mov	r10, qword ptr [rdi + 61016]
	shl	rax, 3
	lea	r11, [rax + 2*rax]
	xor	ebx, ebx
.LBB54_12:                              # =>This Inner Loop Header: Depth=1
	mov	ebp, dword ptr [r10 + rbx + 20]
	xor	eax, eax
	test	ebp, ebp
	je	.LBB54_25
# %bb.13:                               #   in Loop: Header=BB54_12 Depth=1
	mov	r14d, ecx
	sub	r14d, dword ptr [r10 + rbx + 16]
	jb	.LBB54_25
# %bb.14:                               #   in Loop: Header=BB54_12 Depth=1
	cmp	ebp, r14d
	ja	.LBB54_25
# %bb.11:                               #   in Loop: Header=BB54_12 Depth=1
	add	rbx, 24
	cmp	r11, rbx
	jne	.LBB54_12
.LBB54_15:
	mov	rbx, rdx
	mov	ebp, r8d
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 80], xmm0
	mov	rcx, qword ptr [rdi + 8]
	mov	rax, qword ptr [rcx]
	lea	rdx, [rsp + 80]
	mov	qword ptr [rsp + 40], rdx
	mov	dword ptr [rsp + 32], 0
	mov	rdx, r9
	xor	r8d, r8d
	mov	r9d, 4
	call	qword ptr [rax + 112]
	test	eax, eax
	js	.LBB54_23
# %bb.16:
	mov	rcx, qword ptr [rsp + 80]
	mov	rdx, qword ptr [rdi + 60992]
	mov	r8d, dword ptr [rdi + 96]
	shl	r8, 6
	call	memcpy
	mov	rcx, qword ptr [rdi + 8]
	mov	rdx, qword ptr [rdi + 22968]
	mov	rax, qword ptr [rcx]
	xor	r8d, r8d
	call	qword ptr [rax + 120]
	inc	dword ptr [rdi + 99340]
	mov	eax, dword ptr [rdi + 96]
	shl	rax, 6
	add	qword ptr [rdi + 99368], rax
	mov	r8d, ebp
	mov	rdx, rbx
.LBB54_17:
	mov	eax, dword ptr [rdi + 112]
	mov	qword ptr [rsp + 80], rdi
	mov	qword ptr [rsp + 88], rdx
	mov	byte ptr [rsp + 96], 3
	mov	dword ptr [rsp + 100], -1
	vxorps	xmm0, xmm0, xmm0
	vmovups	xmmword ptr [rsp + 104], xmm0
	mov	dword ptr [rsp + 120], 0
	vmovaps	xmmword ptr [rsp + 128], xmm0
	mov	qword ptr [rsp + 144], 0
	sub	esi, r8d
	jbe	.LBB54_22
# %bb.18:
	vcvtsi2ss	xmm0, xmm5, rax
	vdivss	xmm6, xmm0, dword ptr [rip + __real@42c00000]
	mov	eax, r8d
	shl	rax, 3
	lea	r12, [rax + 2*rax]
	lea	rbp, [rsp + 176]
	lea	r14, [rsp + 160]
	jmp	.LBB54_19
	.p2align	4
.LBB54_21:                              #   in Loop: Header=BB54_19 Depth=1
	add	r12, 24
	dec	esi
	je	.LBB54_22
.LBB54_19:                              # =>This Inner Loop Header: Depth=1
	mov	r15, qword ptr [rdi + 61016]
	lea	rdx, [r15 + r12]
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 160], xmm0
	mov	qword ptr [rsp + 40], rbp
	mov	qword ptr [rsp + 32], r14
	mov	rcx, rdi
	vmovaps	xmm2, xmm6
	mov	r9, qword ptr [rsp + 328]
	call	"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z"
	test	al, al
	je	.LBB54_21
# %bb.20:                               #   in Loop: Header=BB54_19 Depth=1
	mov	r13d, dword ptr [r15 + r12 + 16]
	mov	ebx, dword ptr [r15 + r12 + 20]
	mov	r8, r13
	shl	r8, 6
	add	r8, qword ptr [rdi + 60992]
	mov	qword ptr [rsp + 64], r14
	vmovss	dword ptr [rsp + 48], xmm6
	mov	qword ptr [rsp + 40], rbp
	mov	dword ptr [rsp + 32], ebx
	mov	dword ptr [rsp + 56], -1
	mov	rcx, rdi
	mov	rdx, qword ptr [rsp + 328]
	mov	r9d, 64
	call	"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z"
	add	ebx, r13d
	mov	dword ptr [rsp + 32], ebx
	lea	rcx, [rsp + 80]
	lea	rdx, [rsp + 128]
	mov	r8, r14
	mov	r9d, r13d
	call	"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
	mov	eax, dword ptr [r15 + r12 + 20]
	add	dword ptr [rdi + 99320], eax
	jmp	.LBB54_21
.LBB54_22:
	lea	rcx, [rsp + 80]
	lea	rdx, [rsp + 128]
	call	"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
	mov	al, 1
	jmp	.LBB54_25
.LBB54_23:
	lea	rdx, [rip + "??_C@_06IAAOMEKN@render?$AA@"]
	lea	r8, [rip + "??_C@_0CB@JDCMLABG@media_instance_buffer_map_failed@"]
	mov	cl, 3
	call	"?log_write@maicord@@YAXW4LogLevel@1@PEBD1@Z"
	jmp	.LBB54_24
	.seh_endproc
                                        # -- End function
	.def	"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__xmm@7fffffff7fffffff7fffffff7fffffff # -- Begin function ?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z
	.section	.rdata,"dr",discard,__xmm@7fffffff7fffffff7fffffff7fffffff
	.p2align	4, 0x0
__xmm@7fffffff7fffffff7fffffff7fffffff:
	.long	0x7fffffff                      # float NaN
	.long	0x7fffffff                      # float NaN
	.long	0x7fffffff                      # float NaN
	.long	0x7fffffff                      # float NaN
	.globl	__real@49800000
	.section	.rdata,"dr",discard,__real@49800000
	.p2align	2, 0x0
__real@49800000:
	.long	0x49800000                      # float 1048576
	.globl	__real@44800000
	.section	.rdata,"dr",discard,__real@44800000
	.p2align	2, 0x0
__real@44800000:
	.long	0x44800000                      # float 1024
	.globl	__xmm@43300000000000004330000000000000
	.section	.rdata,"dr",discard,__xmm@43300000000000004330000000000000
	.p2align	4, 0x0
__xmm@43300000000000004330000000000000:
	.quad	0x4330000000000000              # double 4503599627370496
	.quad	0x4330000000000000              # double 4503599627370496
	.globl	__real@4330000000000000
	.section	.rdata,"dr",discard,__real@4330000000000000
	.p2align	3, 0x0
__real@4330000000000000:
	.quad	0x4330000000000000              # double 4503599627370496
	.section	.text,"xr",one_only,"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z",unique,56
	.p2align	4
"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z": # @"?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z"
.seh_proc "?batch_scissor@?A0xF64B795B@maicord@@YA_NAEBUD3D11Renderer@2@AEBUUiRect@2@MPEBUtagRECT@@PEAURasterClipRect@2@3@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 104
	.seh_stackalloc 104
	vmovaps	xmmword ptr [rsp + 80], xmm8    # 16-byte Spill
	.seh_savexmm xmm8, 80
	vmovaps	xmmword ptr [rsp + 64], xmm7    # 16-byte Spill
	.seh_savexmm xmm7, 64
	vmovaps	xmmword ptr [rsp + 48], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 48
	.seh_endprologue
	mov	rsi, r9
	vmovaps	xmm6, xmm2
	mov	r8, rdx
	mov	rdx, rcx
	mov	rdi, qword ptr [rsp + 168]
	vmovq	xmm7, qword ptr [rcx + 104]     # xmm7 = mem[0],zero
	lea	rcx, [rsp + 32]
	call	"?layer_clip@?A0xF64B795B@maicord@@YA?AUUiRect@2@AEBUD3D11Renderer@2@AEBU32@@Z"
	vmovss	xmm1, dword ptr [rsp + 32]      # xmm1 = mem[0],zero,zero,zero
	vandps	xmm2, xmm1, xmmword ptr [rip + __xmm@7fffffff7fffffff7fffffff7fffffff]
	vxorps	xmm0, xmm0, xmm0
	vmovss	xmm4, dword ptr [rip + __real@49800000] # xmm4 = [1.048576E+6,0.0E+0,0.0E+0,0.0E+0]
	vucomiss	xmm4, xmm2
	vxorps	xmm5, xmm5, xmm5
	vxorps	xmm8, xmm8, xmm8
	jb	.LBB55_14
# %bb.1:
	vmovss	xmm2, dword ptr [rsp + 36]      # xmm2 = mem[0],zero,zero,zero
	vandps	xmm3, xmm2, xmmword ptr [rip + __xmm@7fffffff7fffffff7fffffff7fffffff]
	vucomiss	xmm4, xmm3
	jb	.LBB55_14
# %bb.2:
	vmovss	xmm3, dword ptr [rsp + 40]      # xmm3 = mem[0],zero,zero,zero
	vandps	xmm5, xmm3, xmmword ptr [rip + __xmm@7fffffff7fffffff7fffffff7fffffff]
	vucomiss	xmm4, xmm5
	vxorps	xmm5, xmm5, xmm5
	jb	.LBB55_14
# %bb.3:
	vmovaps	xmm5, xmm4
	vmovss	xmm4, dword ptr [rsp + 44]      # xmm4 = mem[0],zero,zero,zero
	vandps	xmm8, xmm4, xmmword ptr [rip + __xmm@7fffffff7fffffff7fffffff7fffffff]
	vucomiss	xmm5, xmm8
	vxorps	xmm5, xmm5, xmm5
	vxorps	xmm8, xmm8, xmm8
	jb	.LBB55_14
# %bb.4:
	vpextrd	eax, xmm7, 1
	cmp	eax, 16777216
	ja	.LBB55_14
# %bb.5:
	vmovd	ecx, xmm7
	cmp	ecx, 16777216
	ja	.LBB55_14
# %bb.6:
	test	eax, eax
	je	.LBB55_14
# %bb.7:
	test	ecx, ecx
	je	.LBB55_14
# %bb.8:
	vmovss	xmm5, dword ptr [rip + __real@44800000] # xmm5 = [1.024E+3,0.0E+0,0.0E+0,0.0E+0]
	vucomiss	xmm5, xmm6
	jb	.LBB55_17
# %bb.9:
	vxorps	xmm5, xmm5, xmm5
	vucomiss	xmm6, xmm5
	jbe	.LBB55_17
# %bb.10:
	vucomiss	xmm3, xmm1
	jbe	.LBB55_17
# %bb.11:
	vucomiss	xmm4, xmm2
	vxorps	xmm5, xmm5, xmm5
	jbe	.LBB55_14
# %bb.12:
	vpmovzxdq	xmm0, xmm7              # xmm0 = xmm7[0],zero,xmm7[1],zero
	vmovddup	xmm5, qword ptr [rip + __real@4330000000000000] # xmm5 = [4.503599627370496E+15,4.503599627370496E+15]
                                        # xmm5 = mem[0,0]
	vpor	xmm0, xmm0, xmm5
	vsubpd	xmm0, xmm0, xmm5
	vcvtpd2ps	xmm5, xmm0
	vmovsldup	xmm6, xmm6              # xmm6 = xmm6[0,0,2,2]
	vinsertps	xmm0, xmm1, xmm2, 16    # xmm0 = xmm1[0],xmm2[0],xmm1[2,3]
	vmulps	xmm0, xmm6, xmm0
	vminps	xmm1, xmm0, xmm5
	vxorps	xmm0, xmm0, xmm0
	vcmpnltps	xmm2, xmm1, xmm0
	vandps	xmm1, xmm2, xmm1
	vinsertps	xmm2, xmm3, xmm4, 16    # xmm2 = xmm3[0],xmm4[0],xmm3[2,3]
	vmulps	xmm2, xmm6, xmm2
	vminps	xmm2, xmm2, xmm5
	vcmpnltps	xmm3, xmm2, xmm0
	vandps	xmm2, xmm3, xmm2
	vcmpnltps	xmm3, xmm1, xmm2
	vshufps	xmm3, xmm3, xmm3, 80            # xmm3 = xmm3[0,0,1,1]
	vtestpd	xmm3, xmm3
	vxorps	xmm5, xmm5, xmm5
	jne	.LBB55_14
# %bb.13:
	vcvttps2dq	xmm0, xmm2
	vcvttps2dq	xmm8, xmm1
	vroundps	xmm1, xmm2, 11
	vcmpltps	xmm1, xmm1, xmm2
	vpsubd	xmm0, xmm0, xmm1
	vpunpcklqdq	xmm5, xmm8, xmm0        # xmm5 = xmm8[0],xmm0[0]
.LBB55_14:
	vmovlhps	xmm0, xmm8, xmm0                # xmm0 = xmm8[0],xmm0[0]
	vmovups	xmmword ptr [rdi], xmm5
	vmovaps	xmmword ptr [rsp + 32], xmm0
	lea	rcx, [rsp + 32]
	mov	rdx, rsi
	call	"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z"
	test	al, al
	je	.LBB55_16
# %bb.15:
	mov	rcx, qword ptr [rsp + 160]
	vmovaps	xmm0, xmmword ptr [rsp + 32]
	vmovups	xmmword ptr [rcx], xmm0
.LBB55_16:
	vmovaps	xmm6, xmmword ptr [rsp + 48]    # 16-byte Reload
	vmovaps	xmm7, xmmword ptr [rsp + 64]    # 16-byte Reload
	vmovaps	xmm8, xmmword ptr [rsp + 80]    # 16-byte Reload
	.seh_startepilogue
	add	rsp, 104
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
.LBB55_17:
	vxorps	xmm5, xmm5, xmm5
	jmp	.LBB55_14
	.seh_endproc
                                        # -- End function
	.def	"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z";
	.scl	3;
	.type	32;
	.endef
	.globl	__real@f149f2ca                 # -- Begin function ?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z
	.section	.rdata,"dr",discard,__real@f149f2ca
	.p2align	2, 0x0
__real@f149f2ca:
	.long	0xf149f2ca                      # float -1.00000002E+30
	.globl	__real@7149f2ca
	.section	.rdata,"dr",discard,__real@7149f2ca
	.p2align	2, 0x0
__real@7149f2ca:
	.long	0x7149f2ca                      # float 1.00000002E+30
	.section	.text,"xr",one_only,"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z",unique,57
	.p2align	4
"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z": # @"?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z"
.seh_proc "?widen_scissor@?A0xF64B795B@maicord@@YAXAEBUD3D11Renderer@2@PEBUtagRECT@@PEBXIIAEBURasterClipRect@2@MIPEAU52@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	sub	rsp, 64
	.seh_stackalloc 64
	vmovaps	xmmword ptr [rsp + 48], xmm6    # 16-byte Spill
	.seh_savexmm xmm6, 48
	.seh_endprologue
	vmovss	xmm0, dword ptr [rcx + 120316]  # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, dword ptr [rip + __real@3f800000]
	jne	.LBB56_1
	jnp	.LBB56_2
.LBB56_1:
	vmovaps	xmm6, xmmword ptr [rsp + 48]    # 16-byte Reload
	.seh_startepilogue
	add	rsp, 64
	pop	rsi
	.seh_endepilogue
	ret
.LBB56_2:
	mov	eax, dword ptr [rsp + 112]
	test	eax, eax
	je	.LBB56_1
# %bb.3:
	mov	r11, qword ptr [rsp + 120]
	mov	r10d, dword ptr [r11]
	mov	esi, dword ptr [r11 + 4]
	vmovss	xmm0, dword ptr [rip + __real@f149f2ca] # xmm0 = [-1.00000002E+30,0.0E+0,0.0E+0,0.0E+0]
	vmovaps	xmm1, xmm0
	test	r10d, r10d
	jle	.LBB56_5
# %bb.4:
	vcvtsi2ss	xmm1, xmm5, r10d
.LBB56_5:
	mov	r10d, dword ptr [rcx + 104]
	test	esi, esi
	jle	.LBB56_7
# %bb.6:
	vcvtsi2ss	xmm0, xmm5, esi
.LBB56_7:
	mov	ecx, dword ptr [rcx + 108]
	mov	esi, dword ptr [r11 + 8]
	vmovss	xmm2, dword ptr [rip + __real@7149f2ca] # xmm2 = [1.00000002E+30,0.0E+0,0.0E+0,0.0E+0]
	cmp	esi, r10d
	jl	.LBB56_9
# %bb.8:
	vmovaps	xmm3, xmm2
	mov	esi, dword ptr [rsp + 136]
	mov	r11d, dword ptr [r11 + 12]
	cmp	r11d, ecx
	jl	.LBB56_10
	jmp	.LBB56_11
.LBB56_9:
	vcvtsi2ss	xmm3, xmm5, esi
	mov	esi, dword ptr [rsp + 136]
	mov	r11d, dword ptr [r11 + 12]
	cmp	r11d, ecx
	jge	.LBB56_11
.LBB56_10:
	vcvtsi2ss	xmm2, xmm5, r11d
.LBB56_11:
	vmovss	xmm4, dword ptr [rsp + 128]     # xmm4 = mem[0],zero,zero,zero
	mov	r9d, r9d
	cmp	esi, -1
	je	.LBB56_19
# %bb.12:
	mov	r11d, esi
	vxorps	xmm5, xmm5, xmm5
	.p2align	4
.LBB56_13:                              # =>This Inner Loop Header: Depth=1
	vmulss	xmm6, xmm4, dword ptr [r8]
	vucomiss	xmm6, xmm1
	jb	.LBB56_1
# %bb.14:                               #   in Loop: Header=BB56_13 Depth=1
	vmulss	xmm6, xmm4, dword ptr [r8 + 4]
	vucomiss	xmm6, xmm0
	jb	.LBB56_1
# %bb.15:                               #   in Loop: Header=BB56_13 Depth=1
	vmulss	xmm6, xmm4, dword ptr [r8 + 8]
	vucomiss	xmm3, xmm6
	jb	.LBB56_1
# %bb.16:                               #   in Loop: Header=BB56_13 Depth=1
	vmulss	xmm6, xmm4, dword ptr [r8 + 12]
	vucomiss	xmm2, xmm6
	jb	.LBB56_1
# %bb.17:                               #   in Loop: Header=BB56_13 Depth=1
	vmovss	xmm6, dword ptr [r8 + r11]      # xmm6 = mem[0],zero,zero,zero
	vucomiss	xmm6, xmm5
	jb	.LBB56_1
# %bb.18:                               #   in Loop: Header=BB56_13 Depth=1
	add	r8, r9
	dec	eax
	jne	.LBB56_13
	jmp	.LBB56_25
.LBB56_19:
	add	r8, 12
	.p2align	4
.LBB56_20:                              # =>This Inner Loop Header: Depth=1
	vmulss	xmm5, xmm4, dword ptr [r8 - 12]
	vucomiss	xmm5, xmm1
	jb	.LBB56_1
# %bb.21:                               #   in Loop: Header=BB56_20 Depth=1
	vmulss	xmm5, xmm4, dword ptr [r8 - 8]
	vucomiss	xmm5, xmm0
	jb	.LBB56_1
# %bb.22:                               #   in Loop: Header=BB56_20 Depth=1
	vmulss	xmm5, xmm4, dword ptr [r8 - 4]
	vucomiss	xmm3, xmm5
	jb	.LBB56_1
# %bb.23:                               #   in Loop: Header=BB56_20 Depth=1
	vmulss	xmm5, xmm4, dword ptr [r8]
	vucomiss	xmm2, xmm5
	jb	.LBB56_1
# %bb.24:                               #   in Loop: Header=BB56_20 Depth=1
	add	r8, r9
	dec	eax
	jne	.LBB56_20
.LBB56_25:
	mov	qword ptr [rsp + 32], 0
	mov	dword ptr [rsp + 40], r10d
	mov	dword ptr [rsp + 44], ecx
	lea	rcx, [rsp + 32]
	call	"?intersect_scissor@?A0xF64B795B@maicord@@YA_NPEAUtagRECT@@PEBU3@@Z"
	test	al, al
	je	.LBB56_1
# %bb.26:
	mov	rax, qword ptr [rsp + 144]
	vmovaps	xmm0, xmmword ptr [rsp + 32]
	vmovups	xmmword ptr [rax], xmm0
	vmovaps	xmm6, xmmword ptr [rsp + 48]    # 16-byte Reload
	.seh_startepilogue
	add	rsp, 64
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z",unique,58
	.p2align	4                               # -- Begin function ?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z
"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z": # @"?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
.seh_proc "?append_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@PEAUDrawRun@2@AEBURasterClipRect@2@II@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 40
	.seh_stackalloc 40
	.seh_endprologue
	mov	ebp, dword ptr [rsp + 112]
	mov	eax, ebp
	sub	eax, r9d
	jbe	.LBB57_10
# %bb.1:
	mov	edi, r9d
	mov	rbx, r8
	mov	rsi, rdx
	add	eax, r9d
	jb	.LBB57_8
# %bb.2:
	mov	eax, dword ptr [rsi + 20]
	cmp	eax, edi
	jne	.LBB57_8
# %bb.3:
	cmp	eax, dword ptr [rsi + 16]
	jbe	.LBB57_8
# %bb.4:
	mov	eax, dword ptr [rsi]
	cmp	eax, dword ptr [rbx]
	jne	.LBB57_8
# %bb.5:
	mov	eax, dword ptr [rsi + 4]
	cmp	eax, dword ptr [rbx + 4]
	jne	.LBB57_8
# %bb.6:
	mov	eax, dword ptr [rsi + 8]
	cmp	eax, dword ptr [rbx + 8]
	jne	.LBB57_8
# %bb.7:
	mov	eax, dword ptr [rsi + 12]
	cmp	eax, dword ptr [rbx + 12]
	je	.LBB57_9
.LBB57_8:
	mov	rdx, rsi
	call	"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
	vmovups	xmm0, xmmword ptr [rbx]
	vmovups	xmmword ptr [rsi], xmm0
	mov	dword ptr [rsi + 16], edi
.LBB57_9:
	mov	dword ptr [rsi + 20], ebp
.LBB57_10:
	.seh_startepilogue
	add	rsp, 40
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z",unique,59
	.p2align	4                               # -- Begin function ?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z
"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z": # @"?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
.seh_proc "?draw_run@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@AEBUDrawRun@2@@Z"
# %bb.0:
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 48
	.seh_stackalloc 48
	.seh_endprologue
	mov	eax, dword ptr [rdx + 20]
	cmp	eax, dword ptr [rdx + 16]
	jbe	.LBB58_6
# %bb.1:
	mov	rdi, rdx
	mov	rsi, rcx
	mov	rbx, qword ptr [rcx]
	mov	rdx, qword ptr [rcx + 8]
	movzx	r8d, byte ptr [rcx + 16]
	mov	rcx, rbx
	call	"?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z"
	mov	rdx, qword ptr [rsi + 8]
	mov	rcx, rbx
	mov	r8, rdi
	call	"?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z"
	mov	edx, dword ptr [rdi + 16]
	mov	r8d, dword ptr [rdi + 20]
	mov	ebp, dword ptr [rsi + 36]
	cmp	edx, ebp
	jae	.LBB58_3
# %bb.2:
	mov	r14d, dword ptr [rsi + 32]
	cmp	r8d, r14d
	jbe	.LBB58_3
# %bb.5:
	cmp	edx, r14d
	cmova	r14d, edx
	cmp	r8d, ebp
	cmovb	ebp, r8d
	mov	rcx, rsi
	mov	r8d, r14d
	call	"?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z"
	mov	r8d, ebp
	sub	r8d, r14d
	add	r14d, dword ptr [rsi + 40]
	mov	rcx, qword ptr [rbx + 8]
	sub	r14d, dword ptr [rsi + 32]
	mov	rax, qword ptr [rcx]
	mov	dword ptr [rsp + 32], r14d
	mov	edx, 4
	xor	r9d, r9d
	call	qword ptr [rax + 168]
	inc	dword ptr [rbx + 99312]
	mov	r8d, dword ptr [rdi + 20]
	mov	rcx, rsi
	mov	edx, ebp
	jmp	.LBB58_4
.LBB58_6:
	.seh_startepilogue
	add	rsp, 48
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r14
	.seh_endepilogue
	ret
.LBB58_3:
	mov	rcx, rsi
.LBB58_4:
	.seh_startepilogue
	add	rsp, 48
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r14
	.seh_endepilogue
	jmp	"?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z" # TAILCALL
	.seh_endproc
                                        # -- End function
	.def	"?layer_clip@?A0xF64B795B@maicord@@YA?AUUiRect@2@AEBUD3D11Renderer@2@AEBU32@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?layer_clip@?A0xF64B795B@maicord@@YA?AUUiRect@2@AEBUD3D11Renderer@2@AEBU32@@Z",unique,60
	.p2align	4                               # -- Begin function ?layer_clip@?A0xF64B795B@maicord@@YA?AUUiRect@2@AEBUD3D11Renderer@2@AEBU32@@Z
"?layer_clip@?A0xF64B795B@maicord@@YA?AUUiRect@2@AEBUD3D11Renderer@2@AEBU32@@Z": # @"?layer_clip@?A0xF64B795B@maicord@@YA?AUUiRect@2@AEBUD3D11Renderer@2@AEBU32@@Z"
# %bb.0:
	vmovss	xmm0, dword ptr [rdx + 120316]  # xmm0 = mem[0],zero,zero,zero
	vucomiss	xmm0, dword ptr [rip + __real@3f800000]
	jne	.LBB59_2
	jp	.LBB59_2
# %bb.1:
	vmovups	xmm0, xmmword ptr [r8]
	vmovups	xmmword ptr [rcx], xmm0
	ret
.LBB59_2:
	vmovddup	xmm1, qword ptr [rdx + 120320]  # xmm1 = mem[0,0]
	vmovups	xmm2, xmmword ptr [r8]
	vsubps	xmm2, xmm2, xmm1
	vshufps	xmm0, xmm0, xmm0, 0             # xmm0 = xmm0[0,0,0,0]
	vmulps	xmm0, xmm2, xmm0
	vaddps	xmm0, xmm0, xmm1
	vmovups	xmmword ptr [rcx], xmm0
	ret
                                        # -- End function
	.def	"?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z",unique,61
	.p2align	4                               # -- Begin function ?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z
"?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z": # @"?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z"
.seh_proc "?select_pipeline@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@W4DrawPipeline@2@@Z"
# %bb.0:
	push	r14
	.seh_pushreg r14
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	push	rbp
	.seh_pushreg rbp
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 96
	.seh_stackalloc 96
	.seh_endprologue
	mov	ebp, r8d
	mov	rbx, rdx
	mov	rdi, rcx
	call	"?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z"
	cmp	byte ptr [rbx + 16], bpl
	je	.LBB60_27
# %bb.1:
	mov	byte ptr [rbx + 16], bpl
	mov	byte ptr [rbx + 24], 1
	mov	rsi, qword ptr [rdi + 8]
	mov	dword ptr [rsp + 60], 0
	movzx	eax, byte ptr [rbx + 17]
	cmp	bpl, 2
	je	.LBB60_13
# %bb.2:
	movzx	ecx, bpl
	cmp	ecx, 1
	jne	.LBB60_22
# %bb.3:
	cmp	al, 1
	je	.LBB60_5
# %bb.4:
	mov	byte ptr [rbx + 17], 1
	mov	dword ptr [rsp + 64], 64
	mov	rdx, qword ptr [rdi + 22872]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	call	qword ptr [rax + 136]
	lea	r9, [rdi + 22880]
	mov	rax, qword ptr [rsi]
	lea	rcx, [rsp + 60]
	mov	qword ptr [rsp + 40], rcx
	lea	rcx, [rsp + 64]
	mov	qword ptr [rsp + 32], rcx
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 1
	call	qword ptr [rax + 144]
.LBB60_5:
	mov	rdx, qword ptr [rdi + 22856]
	mov	rax, qword ptr [rsi]
	xor	r14d, r14d
	mov	rcx, rsi
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 88]
	mov	rdx, qword ptr [rdi + 22864]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 72]
	mov	rax, qword ptr [rdi + 22888]
	mov	qword ptr [rsp + 64], rax
	cmp	byte ptr [rdi + 23137], 1
	jne	.LBB60_7
# %bb.6:
	mov	r14, qword ptr [rdi + 23000]
.LBB60_7:
	mov	qword ptr [rsp + 72], r14
	cmp	byte ptr [rbx + 22], 1
	jne	.LBB60_10
# %bb.8:
	cmp	byte ptr [rbx + 23], 1
	jne	.LBB60_10
# %bb.9:
	cmp	qword ptr [rbx + 32], r14
	je	.LBB60_11
.LBB60_10:
	mov	word ptr [rbx + 22], 257
	mov	qword ptr [rbx + 32], r14
	mov	rax, qword ptr [rsi]
	lea	r9, [rsp + 64]
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 2
	call	qword ptr [rax + 128]
.LBB60_11:
	cmp	byte ptr [rbx + 18], 1
	je	.LBB60_27
# %bb.12:
	mov	byte ptr [rbx + 18], 1
	mov	rax, qword ptr [rdi + 23640]
	mov	qword ptr [rsp + 88], rax
	mov	rax, qword ptr [rsi]
	lea	r9, [rsp + 88]
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 1
	jmp	.LBB60_26
.LBB60_13:
	cmp	al, 2
	je	.LBB60_15
# %bb.14:
	mov	byte ptr [rbx + 17], 2
	mov	dword ptr [rsp + 64], 48
	mov	rdx, qword ptr [rdi + 22952]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	call	qword ptr [rax + 136]
	lea	r9, [rdi + 22960]
	mov	rax, qword ptr [rsi]
	lea	rcx, [rsp + 60]
	mov	qword ptr [rsp + 40], rcx
	lea	rcx, [rsp + 64]
	mov	qword ptr [rsp + 32], rcx
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 1
	call	qword ptr [rax + 144]
.LBB60_15:
	mov	rdx, qword ptr [rdi + 22912]
	mov	rax, qword ptr [rsi]
	xor	r14d, r14d
	mov	rcx, rsi
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 88]
	mov	rdx, qword ptr [rdi + 22920]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 72]
	cmp	dword ptr [rdi + 20672], 0
	je	.LBB60_17
# %bb.16:
	mov	r14, qword ptr [rdi + 20664]
.LBB60_17:
	mov	qword ptr [rsp + 88], r14
	cmp	byte ptr [rbx + 23], 1
	jne	.LBB60_19
# %bb.18:
	cmp	qword ptr [rbx + 32], r14
	je	.LBB60_20
.LBB60_19:
	mov	byte ptr [rbx + 23], 1
	mov	qword ptr [rbx + 32], r14
	mov	rax, qword ptr [rsi]
	lea	r9, [rsp + 88]
	mov	rcx, rsi
	mov	edx, 1
	mov	r8d, 1
	call	qword ptr [rax + 128]
.LBB60_20:
	cmp	byte ptr [rbx + 18], 2
	je	.LBB60_27
# %bb.21:
	mov	byte ptr [rbx + 18], 2
	mov	rax, qword ptr [rdi + 23456]
	add	rdi, 23144
	mov	qword ptr [rsp + 64], rax
	mov	rcx, rdi
	call	"?d3d11_glyph_text_color_view@maicord@@YAPEAUID3D11ShaderResourceView@@AEBUD3D11GlyphTextOwner@1@@Z"
	mov	qword ptr [rsp + 72], rax
	mov	rax, qword ptr [rsi]
	lea	r9, [rsp + 64]
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 2
	jmp	.LBB60_26
.LBB60_22:
	cmp	al, bpl
	je	.LBB60_24
# %bb.23:
	mov	byte ptr [rbx + 17], bpl
	mov	dword ptr [rsp + 64], 64
	mov	rdx, qword ptr [rdi + 22944]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	call	qword ptr [rax + 136]
	lea	r9, [rdi + 22968]
	mov	rax, qword ptr [rsi]
	lea	rcx, [rsp + 60]
	mov	qword ptr [rsp + 40], rcx
	lea	rcx, [rsp + 64]
	mov	qword ptr [rsp + 32], rcx
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 1
	call	qword ptr [rax + 144]
.LBB60_24:
	mov	rdx, qword ptr [rdi + 22936]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 88]
	mov	rdx, qword ptr [rdi + 22928]
	mov	rax, qword ptr [rsi]
	mov	rcx, rsi
	xor	r8d, r8d
	xor	r9d, r9d
	call	qword ptr [rax + 72]
	cmp	byte ptr [rbx + 18], bpl
	je	.LBB60_27
# %bb.25:
	mov	byte ptr [rbx + 18], bpl
	mov	rax, qword ptr [rdi + 52280]
	mov	qword ptr [rsp + 64], rax
	mov	rax, qword ptr [rdi + 53080]
	mov	qword ptr [rsp + 72], rax
	mov	rax, qword ptr [rdi + 52288]
	mov	qword ptr [rsp + 80], rax
	mov	rax, qword ptr [rsi]
	lea	r9, [rsp + 64]
	mov	rcx, rsi
	xor	edx, edx
	mov	r8d, 3
.LBB60_26:
	call	qword ptr [rax + 64]
	nop
.LBB60_27:
	.seh_startepilogue
	add	rsp, 96
	pop	rbx
	pop	rbp
	pop	rdi
	pop	rsi
	pop	r14
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z",unique,62
	.p2align	4                               # -- Begin function ?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z
"?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z": # @"?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z"
.seh_proc "?set_scissor@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBURasterClipRect@2@@Z"
# %bb.0:
	sub	rsp, 56
	.seh_stackalloc 56
	.seh_endprologue
	cmp	byte ptr [rdx + 20], 1
	jne	.LBB61_5
# %bb.1:
	mov	eax, dword ptr [rdx]
	cmp	eax, dword ptr [r8]
	jne	.LBB61_5
# %bb.2:
	mov	eax, dword ptr [rdx + 4]
	cmp	eax, dword ptr [r8 + 4]
	jne	.LBB61_5
# %bb.3:
	mov	eax, dword ptr [rdx + 8]
	cmp	eax, dword ptr [r8 + 8]
	jne	.LBB61_5
# %bb.4:
	mov	eax, dword ptr [rdx + 12]
	cmp	eax, dword ptr [r8 + 12]
	je	.LBB61_6
.LBB61_5:
	vmovups	xmm0, xmmword ptr [r8]
	vmovups	xmmword ptr [rdx], xmm0
	mov	byte ptr [rdx + 20], 1
	mov	eax, dword ptr [rcx + 120524]
	mov	edx, dword ptr [r8]
	mov	dword ptr [rsp + 40], edx
	mov	edx, dword ptr [r8 + 4]
	add	edx, eax
	mov	dword ptr [rsp + 44], edx
	mov	edx, dword ptr [r8 + 8]
	mov	dword ptr [rsp + 48], edx
	add	eax, dword ptr [r8 + 12]
	mov	dword ptr [rsp + 52], eax
	mov	rcx, qword ptr [rcx + 8]
	mov	rax, qword ptr [rcx]
	lea	r8, [rsp + 40]
	mov	edx, 1
	call	qword ptr [rax + 360]
	nop
.LBB61_6:
	.seh_startepilogue
	add	rsp, 56
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z",unique,63
	.p2align	4                               # -- Begin function ?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z
"?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z": # @"?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z"
.seh_proc "?draw_instances@?A0xF64B795B@maicord@@YAXAEBURunTarget@12@II@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 72
	.seh_stackalloc 72
	.seh_endprologue
	cmp	edx, r8d
	jae	.LBB62_5
# %bb.1:
	mov	rdi, qword ptr [rcx]
	mov	rsi, qword ptr [rdi + 8]
	mov	eax, dword ptr [rcx + 20]
	cmp	edx, eax
	jae	.LBB62_3
# %bb.2:
	mov	r9d, dword ptr [rcx + 24]
	add	r9d, edx
	cmp	r8d, eax
	cmovb	eax, r8d
	sub	eax, edx
	mov	r10, qword ptr [rsi]
	mov	dword ptr [rsp + 32], r9d
	mov	qword ptr [rsp + 64], rcx       # 8-byte Spill
	mov	rcx, rsi
	mov	qword ptr [rsp + 56], rdi       # 8-byte Spill
	mov	edi, edx
	mov	edx, 4
	mov	dword ptr [rsp + 52], r8d       # 4-byte Spill
	mov	r8d, eax
	xor	r9d, r9d
	call	qword ptr [r10 + 168]
	mov	rcx, qword ptr [rsp + 64]       # 8-byte Reload
	mov	edx, edi
	mov	rdi, qword ptr [rsp + 56]       # 8-byte Reload
	mov	r8d, dword ptr [rsp + 52]       # 4-byte Reload
	inc	dword ptr [rdi + 99312]
	mov	eax, dword ptr [rcx + 20]
.LBB62_3:
	cmp	r8d, eax
	jbe	.LBB62_5
# %bb.4:
	cmp	edx, eax
	cmovbe	edx, eax
	sub	r8d, edx
	sub	edx, eax
	add	edx, dword ptr [rcx + 28]
	mov	rax, qword ptr [rsi]
	mov	dword ptr [rsp + 32], edx
	mov	rcx, rsi
	mov	edx, 4
	xor	r9d, r9d
	call	qword ptr [rax + 168]
	inc	dword ptr [rdi + 99312]
.LBB62_5:
	.seh_startepilogue
	add	rsp, 72
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z",unique,64
	.p2align	4                               # -- Begin function ?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z
"?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z": # @"?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z"
.seh_proc "?bind_shared_state@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@@Z"
# %bb.0:
	push	rsi
	.seh_pushreg rsi
	push	rdi
	.seh_pushreg rdi
	sub	rsp, 56
	.seh_stackalloc 56
	.seh_endprologue
	mov	rsi, rcx
	mov	rdi, qword ptr [rcx + 8]
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 32], xmm0
	cmp	byte ptr [rdx + 19], 1
	jne	.LBB63_4
# %bb.1:
	movzx	eax, byte ptr [rdx + 21]
	mov	byte ptr [rdx + 21], 0
	cmp	al, 1
	jne	.LBB63_3
# %bb.2:
	mov	rdx, qword ptr [rsi + 22904]
	mov	rax, qword ptr [rdi]
	lea	r8, [rsp + 32]
	mov	rcx, rdi
	mov	r9d, -1
	call	qword ptr [rax + 280]
	nop
.LBB63_3:
	.seh_startepilogue
	add	rsp, 56
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
.LBB63_4:
	mov	byte ptr [rdx + 19], 1
	mov	byte ptr [rdx + 21], 0
	lea	r8, [rsi + 56]
	mov	rax, qword ptr [rdi]
	mov	rcx, rdi
	mov	edx, 1
	xor	r9d, r9d
	call	qword ptr [rax + 264]
	mov	rax, qword ptr [rdi]
	mov	rcx, rdi
	mov	edx, 5
	call	qword ptr [rax + 192]
	mov	rdx, qword ptr [rsi + 22896]
	mov	rax, qword ptr [rdi]
	mov	rcx, rdi
	call	qword ptr [rax + 344]
	mov	rdx, qword ptr [rsi + 22904]
	mov	rax, qword ptr [rdi]
	lea	r8, [rsp + 32]
	mov	rcx, rdi
	mov	r9d, -1
	call	qword ptr [rax + 280]
	lea	r9, [rsi + 22888]
	mov	rax, qword ptr [rdi]
	mov	rcx, rdi
	xor	edx, edx
	mov	r8d, 1
	call	qword ptr [rax + 56]
	add	rsi, 22976
	mov	rax, qword ptr [rdi]
	mov	rcx, rdi
	xor	edx, edx
	mov	r8d, 1
	mov	r9, rsi
	call	qword ptr [rax + 80]
	nop
	.seh_startepilogue
	add	rsp, 56
	pop	rdi
	pop	rsi
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z",unique,65
	.p2align	4                               # -- Begin function ??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z
"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z": # @"??$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z"
# %bb.0:
	mov	qword ptr [rcx], rdx
	lea	rax, [rip + "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z"]
	mov	qword ptr [rcx + 8], rax
	ret
                                        # -- End function
	.def	"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z",unique,66
	.p2align	4                               # -- Begin function ?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z
"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z": # @"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z"
.seh_proc "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z"
# %bb.0:
	sub	rsp, 56
	.seh_stackalloc 56
	.seh_endprologue
	mov	r9d, r8d
	mov	r8d, edx
	mov	rax, qword ptr [rcx + 24]
	mov	r10, qword ptr [rax]
	mov	rax, qword ptr [rcx + 16]
	vmovss	xmm0, dword ptr [rax]           # xmm0 = mem[0],zero,zero,zero
	mov	rax, qword ptr [rcx]
	mov	rdx, qword ptr [rcx + 8]
	mov	qword ptr [rsp + 40], r10
	vmovss	dword ptr [rsp + 32], xmm0
	mov	rcx, rax
	call	"?draw_paint_batches@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@IIMPEBUtagRECT@@@Z"
	nop
	.seh_startepilogue
	add	rsp, 56
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z",unique,67
	.p2align	4                               # -- Begin function ??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z
"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z": # @"??$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@01@PEAV<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@1@YA_NPEAUD3D11Renderer@1@AEBUTheme@1@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z"
# %bb.0:
	mov	qword ptr [rcx], rdx
	lea	rax, [rip + "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"]
	mov	qword ptr [rcx + 8], rax
	ret
                                        # -- End function
	.def	"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z",unique,68
	.p2align	4                               # -- Begin function ?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z
"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z": # @"?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"
.seh_proc "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"
# %bb.0:
	push	rbx
	.seh_pushreg rbx
	sub	rsp, 64
	.seh_stackalloc 64
	.seh_endprologue
	mov	r9d, edx
	mov	rax, qword ptr [rcx + 32]
	mov	r11, qword ptr [rax]
	mov	rax, qword ptr [rcx + 24]
	movzx	ebx, byte ptr [rax]
	mov	rax, qword ptr [rcx + 16]
	mov	r10, qword ptr [rcx]
	mov	rdx, qword ptr [rcx + 8]
	mov	qword ptr [rsp + 48], r11
	mov	byte ptr [rsp + 40], bl
	mov	dword ptr [rsp + 32], r8d
	mov	rcx, r10
	mov	r8, rax
	call	"?draw_gpu_text@?A0xF64B795B@maicord@@YA_NPEAUD3D11Renderer@2@PEAUDrawSubmissionState@2@AEBUTheme@2@II_NPEBUtagRECT@@@Z"
	nop
	.seh_startepilogue
	add	rsp, 64
	pop	rbx
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.def	"?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z",unique,69
	.p2align	4                               # -- Begin function ?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z
"?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z": # @"?scene_node@?A0xF64B795B@maicord@@YAPEBUUiLayoutNode@2@PEBUD3D11Renderer@2@W4UiSemantic@2@@Z"
# %bb.0:
	test	rcx, rcx
	je	.LBB68_1
# %bb.2:
	mov	dl, 7
	jmp	"?ui_scene_node@maicord@@YAPEBUUiLayoutNode@1@AEBUUiScene@1@W4UiSemantic@1@@Z" # TAILCALL
.LBB68_1:
	xor	eax, eax
	ret
                                        # -- End function
	.def	"?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z";
	.scl	3;
	.type	32;
	.endef
	.section	.text,"xr",one_only,"?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z",unique,70
	.p2align	4                               # -- Begin function ?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z
"?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z": # @"?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z"
.seh_proc "?release_stream_views@?A0xF64B795B@maicord@@YAXPEAUD3D11Renderer@2@AEBUDrawSubmissionState@2@@Z"
# %bb.0:
	sub	rsp, 56
	.seh_stackalloc 56
	.seh_endprologue
	cmp	dl, 1
	jne	.LBB69_2
# %bb.1:
	vxorps	xmm0, xmm0, xmm0
	vmovaps	xmmword ptr [rsp + 32], xmm0
	mov	qword ptr [rsp + 48], 0
	mov	rcx, qword ptr [rcx + 8]
	mov	rax, qword ptr [rcx]
	lea	r9, [rsp + 32]
	xor	edx, edx
	mov	r8d, 3
	call	qword ptr [rax + 64]
	nop
.LBB69_2:
	.seh_startepilogue
	add	rsp, 56
	.seh_endepilogue
	ret
	.seh_endproc
                                        # -- End function
	.section	.rdata,"dr",discard,"??_C@_0CB@HLLCFHNL@ui_instance_buffer_growth_failed@"
	.globl	"??_C@_0CB@HLLCFHNL@ui_instance_buffer_growth_failed@" # @"??_C@_0CB@HLLCFHNL@ui_instance_buffer_growth_failed@"
	.p2align	3, 0x0
"??_C@_0CB@HLLCFHNL@ui_instance_buffer_growth_failed@":
	.asciz	"ui_instance_buffer_growth_failed"

	.section	.rdata,"dr",discard,"??_C@_06IAAOMEKN@render?$AA@"
	.globl	"??_C@_06IAAOMEKN@render?$AA@"  # @"??_C@_06IAAOMEKN@render?$AA@"
	.p2align	2, 0x0
"??_C@_06IAAOMEKN@render?$AA@":
	.asciz	"render"

	.section	.rdata,"dr",discard,"??_C@_0BJ@MLLFCGOM@ui_instance_native_error?$AA@"
	.globl	"??_C@_0BJ@MLLFCGOM@ui_instance_native_error?$AA@" # @"??_C@_0BJ@MLLFCGOM@ui_instance_native_error?$AA@"
	.p2align	3, 0x0
"??_C@_0BJ@MLLFCGOM@ui_instance_native_error?$AA@":
	.asciz	"ui_instance_native_error"

	.section	.rdata,"dr",discard,"??_C@_0BO@DKOJHADH@ui_instance_buffer_map_failed?$AA@"
	.globl	"??_C@_0BO@DKOJHADH@ui_instance_buffer_map_failed?$AA@" # @"??_C@_0BO@DKOJHADH@ui_instance_buffer_map_failed?$AA@"
	.p2align	3, 0x0
"??_C@_0BO@DKOJHADH@ui_instance_buffer_map_failed?$AA@":
	.asciz	"ui_instance_buffer_map_failed"

	.section	.rdata,"dr",discard,"??_C@_0BJ@MGLHKPME@icon_atlas_raster_failed?$AA@"
	.globl	"??_C@_0BJ@MGLHKPME@icon_atlas_raster_failed?$AA@" # @"??_C@_0BJ@MGLHKPME@icon_atlas_raster_failed?$AA@"
	.p2align	3, 0x0
"??_C@_0BJ@MGLHKPME@icon_atlas_raster_failed?$AA@":
	.asciz	"icon_atlas_raster_failed"

	.section	.rdata,"dr",discard,_GUID_6f15aaf2_d208_4e89_9ab4_489535d34f9c
	.globl	_GUID_6f15aaf2_d208_4e89_9ab4_489535d34f9c # @_GUID_6f15aaf2_d208_4e89_9ab4_489535d34f9c
	.p2align	3, 0x0
_GUID_6f15aaf2_d208_4e89_9ab4_489535d34f9c:
	.long	1863690994                      # 0x6f15aaf2
	.short	53768                           # 0xd208
	.short	20105                           # 0x4e89
	.ascii	"\232\264H\2255\323O\234"

	.section	.rdata,"dr",discard,"??_C@_0CB@JDCMLABG@media_instance_buffer_map_failed@"
	.globl	"??_C@_0CB@JDCMLABG@media_instance_buffer_map_failed@" # @"??_C@_0CB@JDCMLABG@media_instance_buffer_map_failed@"
	.p2align	3, 0x0
"??_C@_0CB@JDCMLABG@media_instance_buffer_map_failed@":
	.asciz	"media_instance_buffer_map_failed"

	.section	.rdata,"dr",discard,"??_C@_0CB@JFPKHMFH@glyph_instance_buffer_map_failed@"
	.globl	"??_C@_0CB@JFPKHMFH@glyph_instance_buffer_map_failed@" # @"??_C@_0CB@JFPKHMFH@glyph_instance_buffer_map_failed@"
	.p2align	3, 0x0
"??_C@_0CB@JFPKHMFH@glyph_instance_buffer_map_failed@":
	.asciz	"glyph_instance_buffer_map_failed"

	.section	.debug$S,"dr"
	.p2align	2, 0x0
	.long	4                               # Debug section magic
	.long	241
	.long	.Ltmp1-.Ltmp0                   # Subsection size
.Ltmp0:
	.short	.Ltmp3-.Ltmp2                   # Record length
.Ltmp2:
	.short	4353                            # Record kind: S_OBJNAME
	.long	0                               # Signature
	.byte	0                               # Object name
	.p2align	2, 0x0
.Ltmp3:
	.short	.Ltmp5-.Ltmp4                   # Record length
.Ltmp4:
	.short	4412                            # Record kind: S_COMPILE3
	.long	1                               # Flags and language
	.short	208                             # CPUType
	.short	24                              # Frontend version
	.short	0
	.short	0
	.short	0
	.short	24000                           # Backend version
	.short	0
	.short	0
	.short	0
	.asciz	"clang version 24.0.0git (https://github.com/maj113/llvm-pe 90effcbeb3f6ac2ddd7d55d7a190ff5968b6575a)" # Null-terminated compiler version string
	.p2align	2, 0x0
.Ltmp5:
.Ltmp1:
	.p2align	2, 0x0
	.addrsig
	.addrsig_sym "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_2>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"
	.addrsig_sym "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_3>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II@Z@@Z@CA?A?<auto>@@PEAXII@Z"
	.addrsig_sym "?__invoke@<lambda_1>@?0???$batch_draw@V<lambda_1>@?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@maicord@@YA_NPEAUD3D11Renderer@4@AEBUTheme@4@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@?A0xF64B795B@maicord@@YA?AUBatchDraw@23@PEAV1?0???R<lambda_5>@?0??d3d11_renderer_draw_stream@3@YA_NPEAUD3D11Renderer@3@AEBUTheme@3@_N2PEBUtagRECT@@_K@Z@QEBA?A?<auto>@@II2@Z@@Z@CA?A?<auto>@@PEAXII@Z"
	.addrsig_sym _GUID_6f15aaf2_d208_4e89_9ab4_489535d34f9c
	.globl	_fltused
