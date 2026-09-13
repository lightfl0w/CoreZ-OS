        org     0x7C00
        cli
        cld
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     es, ax
        mov     [0x0FFE], dl

        mov     si, 0x7C00 + 0x1BE
        mov     cx, 4
.find:
        cmp     byte [si], 0x80
        je      .have
        add     si, 16
        loop    .find
        mov     si, 0x7C00 + 0x1BE
.have:
        mov     eax, [si + 8]
        mov     [dap + 8], eax
        mov     si, dap
        mov     dl, byte [0x0FFE]
        mov     ah, 0x42
        int     0x13
        jc      err
        jmp     word 0x0000:0x0600

err:
        mov     si, msg
.put:
        lodsb
        test    al, al
        jz      .h
        mov     ah, 0x0E
        mov     bx, 15
        int     0x10
        jmp     .put
.h:
        hlt
        jmp     .h

msg:    db      0x0A, 0x0A, "load error", 0

        align   8
dap:    db      0x10
        db      0
        dw      1
        dw      0x0000
        dw      0x0060
        dd      0, 0

        times   510-($-$$) db 0
        db      0x55, 0xAA
