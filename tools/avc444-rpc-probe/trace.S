.syntax unified
.thumb
.text
.macro trace name, number
.global \name
.type \name, %function
.thumb_func
\name:
    push {r0-r3, r4, lr}
    mov r1, sp
    movs r0, #\number
    bl rpc_trace_enter
    mov r12, r0
    pop {r0-r3, r4, lr}
    bx r12
.size \name, .-\name
.endm
trace vrpc_write, 0
trace vrpc_inband_pts2, 1
trace vrpc_inband_decode, 2
trace vrpc_composer_enqueue, 3
trace vrpc_composer_dequeue, 4
trace DirectVoGetFrame, 5
trace DirectVoUpdateFrameInfo, 6
trace DirectVoUpdateWriteIndex, 7
trace DirectVoRelease, 8
trace vrpc_composer_connect_decoder, 9
trace vrpc_composer_connect_dvo, 10
.section .note.GNU-stack,"",%progbits
