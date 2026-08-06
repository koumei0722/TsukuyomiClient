.code

extern tsukuyomiCameraHook : proc
extern tsukuyomiCameraTrampoline : qword

tsukuyomiCameraTrampolineEntry proc
    push rbp
    mov rbp, rsp
    and rsp, -10h

    push rax
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11

    sub rsp, 60h
    movdqu [rsp + 00h], xmm0
    movdqu [rsp + 10h], xmm1
    movdqu [rsp + 20h], xmm2
    movdqu [rsp + 30h], xmm3
    movdqu [rsp + 40h], xmm4
    movdqu [rsp + 50h], xmm5

    sub rsp, 20h
    mov rcx, rdi
    call tsukuyomiCameraHook
    add rsp, 20h

    movdqu xmm0, [rsp + 00h]
    movdqu xmm1, [rsp + 10h]
    movdqu xmm2, [rsp + 20h]
    movdqu xmm3, [rsp + 30h]
    movdqu xmm4, [rsp + 40h]
    movdqu xmm5, [rsp + 50h]
    add rsp, 60h

    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    pop rax

    mov rsp, rbp
    pop rbp

    jmp qword ptr [tsukuyomiCameraTrampoline]
tsukuyomiCameraTrampolineEntry endp

extern tsukuyomiPlayerViewHook : proc
extern tsukuyomiPlayerViewTrampoline : qword

tsukuyomiPlayerViewTrampolineEntry proc
    push rbp
    mov rbp, rsp
    and rsp, -10h

    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11

    sub rsp, 60h
    movdqu [rsp + 00h], xmm0
    movdqu [rsp + 10h], xmm1
    movdqu [rsp + 20h], xmm2
    movdqu [rsp + 30h], xmm3
    movdqu [rsp + 40h], xmm4
    movdqu [rsp + 50h], xmm5

    sub rsp, 20h

    call tsukuyomiPlayerViewHook
    add rsp, 20h

    movdqu xmm0, [rsp + 00h]
    movdqu xmm1, [rsp + 10h]
    movdqu xmm2, [rsp + 20h]
    movdqu xmm3, [rsp + 30h]
    movdqu xmm4, [rsp + 40h]
    movdqu xmm5, [rsp + 50h]
    add rsp, 60h

    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx

    mov rsp, rbp
    pop rbp

    jmp qword ptr [tsukuyomiPlayerViewTrampoline]
tsukuyomiPlayerViewTrampolineEntry endp

extern tsukuyomiShouldBlockPacket : proc
extern tsukuyomiPacketSendTrampoline : qword

tsukuyomiPacketSendTrampolineEntry proc
    push rbp
    mov rbp, rsp
    and rsp, -10h

    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11

    sub rsp, 60h
    movdqu [rsp + 00h], xmm0
    movdqu [rsp + 10h], xmm1
    movdqu [rsp + 20h], xmm2
    movdqu [rsp + 30h], xmm3
    movdqu [rsp + 40h], xmm4
    movdqu [rsp + 50h], xmm5

    sub rsp, 20h
    mov rcx, rdx
    call tsukuyomiShouldBlockPacket
    add rsp, 20h

    movdqu xmm0, [rsp + 00h]
    movdqu xmm1, [rsp + 10h]
    movdqu xmm2, [rsp + 20h]
    movdqu xmm3, [rsp + 30h]
    movdqu xmm4, [rsp + 40h]
    movdqu xmm5, [rsp + 50h]
    add rsp, 60h

    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx

    mov rsp, rbp
    pop rbp

    test al, al
    jnz packetBlocked

    jmp qword ptr [tsukuyomiPacketSendTrampoline]

packetBlocked:
    xor eax, eax
    ret
tsukuyomiPacketSendTrampolineEntry endp

end
