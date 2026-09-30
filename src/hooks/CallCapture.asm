.code

PUBLIC tsukuyomiCallCaptureXmm3
tsukuyomiCallCaptureXmm3 proc frame
    push rbx
    .pushreg rbx
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rbx, r9
    mov rax, rcx
    mov rcx, rdx
    mov edx, r8d
    call rax
    movss dword ptr [rbx], xmm3
    add rsp, 20h
    pop rbx
    ret
tsukuyomiCallCaptureXmm3 endp

end
