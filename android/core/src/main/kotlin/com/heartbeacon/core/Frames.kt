package com.heartbeacon.core

/** BLE fragmentation, protocol/PROTOCOL.md section 6.1. */
object Frames {
    const val MIN_MTU = 23
    const val TIMEOUT_MS = 2000L
    const val MORE = 0x80

    /** Frames of [msg] for this [mtu]; [seq] is the message sequence number 0..127. */
    fun split(msg: ByteArray, seq: Int, mtu: Int): List<ByteArray> {
        require(mtu >= MIN_MTU && msg.size in 1..Envelope.MAX_BYTES && seq in 0..127) { "bad_arg" }
        val n = mtu - 4
        val chunks = (msg.indices step n).map { msg.copyOfRange(it, minOf(it + n, msg.size)) }
        return chunks.mapIndexed { i, c -> byteArrayOf((seq or if (i < chunks.lastIndex) MORE else 0).toByte()) + c }
    }
}

/** Receiver side of section 6.1. Time is injected; one instance per link, [reset] on disconnect. */
class Reassembler {
    enum class Out { PARTIAL, COMPLETE, SKIP, OVERSIZE, EMPTY }

    /** [msg] is set only for [Out.COMPLETE]. */
    class Result(val out: Out, val msg: ByteArray? = null)

    private enum class State { IDLE, COLLECT, SKIP }

    private var state = State.IDLE
    private var seq = 0
    private var last = 0L
    private val buf = ByteArray(Envelope.MAX_BYTES)
    private var len = 0

    fun reset() {
        state = State.IDLE
        len = 0
    }

    fun push(frame: ByteArray, nowMs: Long): Result {
        if (frame.isEmpty()) return Result(Out.EMPTY)
        val fseq = frame[0].toInt() and 0x7F
        val more = frame[0].toInt() and Frames.MORE != 0
        val n = frame.size - 1

        if (state != State.IDLE && ((nowMs - last) and 0xFFFFFFFFL) > Frames.TIMEOUT_MS) {
            state = if (fseq == seq) State.SKIP else State.IDLE
        }
        if (state != State.IDLE && fseq != seq) state = State.IDLE
        if (state == State.SKIP) {
            last = nowMs
            if (!more) state = State.IDLE
            return Result(Out.SKIP)
        }
        if (state == State.IDLE) {
            state = State.COLLECT
            seq = fseq
            len = 0
        }
        last = nowMs
        if (len + n > buf.size) {
            state = if (more) State.SKIP else State.IDLE
            len = 0
            return Result(Out.OVERSIZE)
        }
        frame.copyInto(buf, len, 1)
        len += n
        if (more) return Result(Out.PARTIAL)
        state = State.IDLE
        return Result(Out.COMPLETE, buf.copyOf(len))
    }
}
