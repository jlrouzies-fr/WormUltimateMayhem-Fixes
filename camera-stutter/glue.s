# Entry stubs for camfix. Assembled for i686 COFF, Intel syntax.
# Each stub is reached from a patched instruction in WormsMayhem.exe.
# Original targets are reached with push/ret so no relocation is needed.

    .intel_syntax noprefix
    .text

# Replaces `call 0x68d5ee` at 0x68d4f0 inside TaskManager step (0x68d4d4).
# ebp is that function's frame; [ebp+8] points at the game clock (ms).
    .globl _glue_frame_begin
_glue_frame_begin:
    pushad
    mov eax, dword ptr [ebp + 8]
    push dword ptr [eax]
    call _camfix_frame_begin
    add esp, 4
    popad
    push 0x68d5ee
    ret

# Replaces `call 0x68d91f` at 0x68d5ac (first per-frame task list), same frame.
    .globl _glue_frame_present
_glue_frame_present:
    pushad
    mov eax, dword ptr [ebp + 8]
    push dword ptr [eax]
    call _camfix_frame_present
    add esp, 4
    popad
    push 0x68d91f
    ret

# Replaces ZCamUpdateFudgeService::Run at 0x533c90.
# Stack on entry: [esp+4] = this, [esp+8] = task time. Returns delay in eax, ret 8.
    .globl _glue_camera_tick
_glue_camera_tick:
    push dword ptr [esp + 8]
    call _camfix_camera_tick
    add esp, 4
    ret 8

# Replaces `call dword ptr [SwapBuffers]` at 0x795548 (stdcall, one argument).
    .globl _glue_swap
_glue_swap:
    push dword ptr [esp + 4]
    call _camfix_swap
    add esp, 4
    ret 4
