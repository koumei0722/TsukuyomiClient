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
    mov rdx, rbx
    lea r8, [rbp + 8]
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

extern tsukuyomiOnPacketSend : proc
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
    xchg rcx, rdx
    call tsukuyomiOnPacketSend
    add rsp, 20h
    mov r10, rax

    movdqu xmm0, [rsp + 00h]
    movdqu xmm1, [rsp + 10h]
    movdqu xmm2, [rsp + 20h]
    movdqu xmm3, [rsp + 30h]
    movdqu xmm4, [rsp + 40h]
    movdqu xmm5, [rsp + 50h]
    add rsp, 60h

    test r10, r10
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx

    mov rsp, rbp
    pop rbp
    jz packet_send_go
    xor eax, eax
    ret
packet_send_go:

    jmp qword ptr [tsukuyomiPacketSendTrampoline]

tsukuyomiPacketSendTrampolineEntry endp

extern tsukuyomiSessionTickFinished : proc
extern tsukuyomiSessionTickFirstTrampoline : qword
extern tsukuyomiSessionTickCatchupTrampoline : qword

SESSION_TICK_FINISH macro entryName, sessionReg, branchNumber, trampolineName
entryName proc
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 80h
    movdqu [rsp + 20h], xmm0
    movdqu [rsp + 30h], xmm1
    movdqu [rsp + 40h], xmm2
    movdqu [rsp + 50h], xmm3
    movdqu [rsp + 60h], xmm4
    movdqu [rsp + 70h], xmm5
    mov rcx, sessionReg
    mov edx, branchNumber
    call tsukuyomiSessionTickFinished
    movdqu xmm0, [rsp + 20h]
    movdqu xmm1, [rsp + 30h]
    movdqu xmm2, [rsp + 40h]
    movdqu xmm3, [rsp + 50h]
    movdqu xmm4, [rsp + 60h]
    movdqu xmm5, [rsp + 70h]
    add rsp, 80h
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq
    jmp qword ptr [trampolineName]
entryName endp
endm

SESSION_TICK_FINISH tsukuyomiSessionTickFirstEntry, r14, 1, tsukuyomiSessionTickFirstTrampoline
SESSION_TICK_FINISH tsukuyomiSessionTickCatchupEntry, r15, 2, tsukuyomiSessionTickCatchupTrampoline

end
