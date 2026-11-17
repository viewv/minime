/* stub.S — 未实现导入的统一处理函数
 * 桩把 "dll!函数名" 的指针放在 r10 里跳过来。这里按 Windows 调用约定调用
 * unknown_import_log(r10)（ms_abi，会自己保存 rsi/rdi/xmm6-15），然后返回 0。 */
    .intel_syntax noprefix
    .text
    .globl unknown_import_handler
    .type unknown_import_handler, @function
unknown_import_handler:
    sub rsp, 40              /* 32 字节影子空间 + 对齐 */
    mov rcx, r10
    call unknown_import_log
    add rsp, 40
    xor eax, eax
    ret
    .size unknown_import_handler, .-unknown_import_handler
    .section .note.GNU-stack,"",@progbits
