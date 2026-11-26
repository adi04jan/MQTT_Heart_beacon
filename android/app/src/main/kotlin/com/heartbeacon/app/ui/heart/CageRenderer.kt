package com.heartbeacon.app.ui.heart

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/** How the beacon is: lit and normal, asleep (dimmed, cool), or unreachable (a desaturated ghost). */
enum class Look { AWAKE, ASLEEP, OFFLINE }

/**
 * Draws [Cage] as the real object, hand-soldered: thin brass rods shaded as cylinders with a soft, wide highlight,
 * running into rounded beads of tin of uneven size, each one smooth dome drawn over its rod ends; the black battery block with
 * the blue board and USB-C socket under it and the orange button on its back; the 9 LED packages at the angles they
 * were glued at, chained by copper wire; and the LEDs' light on the rods, the block and the cloth below.
 *
 * Everything is depth-sorted back to front each frame. Only draws when asked, so an idle beacon costs nothing.
 */
class CageRenderer {
    private val n = Cage.nodeCount
    private val r = Cage.rodCount

    // Static LED light falloff on every rod midpoint and node: 1 / (1 + 9 d^2) in model space.
    private val rodSpill = FloatArray(r * 9)
    private val nodeSpill = FloatArray(n * 9)

    // Hand-made unevenness: each rod's patina, each joint's size.
    private val rodTone = FloatArray(r) { 0.80f + 0.26f * hash(it * 7 + 3) }
    private val nodeSize = FloatArray(n) { 0.86f + 0.44f * hash(it * 13 + 5) }

    // Per frame: nodes in view space, on screen, and their lit colour.
    private val vx = FloatArray(n); private val vy = FloatArray(n); private val vz = FloatArray(n)
    private val sx = FloatArray(n); private val sy = FloatArray(n); private val ss = FloatArray(n)
    private val nr = FloatArray(n); private val ng = FloatArray(n); private val nb = FloatArray(n)
    private val front = FloatArray(n)

    // Quads (block faces, board, socket, LED packages): model corners, then per frame view and screen corners.
    private val quad = FloatArray(Q * 12)
    private val qvx = FloatArray(Q * 4); private val qvy = FloatArray(Q * 4); private val qvz = FloatArray(Q * 4)
    private val qsx = FloatArray(Q * 4); private val qsy = FloatArray(Q * 4)

    // Copper chain wire: three model points each (end, sagging middle, end), then on screen.
    private val wire = FloatArray(WIRES * 9)
    private val wsx = FloatArray(WIRES * 3); private val wsy = FloatArray(WIRES * 3); private val wz = FloatArray(WIRES)

    // The heart's outline at mid depth, for the wave a sent mood leaves.
    private val outline = FloatArray(OUTLINE * 2)

    private val order = IntArray(r + n + Q + 1 + WIRES)
    private val light = FloatArray(27)
    private val path = Path()
    private var tx = 0f; private var ty = 0f; private var tz = 0f

    private val glow = Brush.radialGradient(
        0f to Color.White, 0.22f to Color(1f, 1f, 1f, 0.55f), 0.5f to Color(1f, 1f, 1f, 0.17f), 0.78f to Color(1f, 1f, 1f, 0.04f), 1f to Color.Transparent,
        center = Offset.Zero, radius = 1f,
    )
    private val dome = Brush.radialGradient(
        0f to Color.White, 0.3f to Color(0.84f, 0.84f, 0.84f), 0.62f to Color(0.6f, 0.6f, 0.61f), 1f to Color(0.34f, 0.34f, 0.36f),
        center = Offset(-0.3f, -0.34f), radius = 1.5f,
    )
    private val shade = Brush.radialGradient(
        0f to Color(0f, 0f, 0f, 0.62f), 0.6f to Color(0f, 0f, 0f, 0.22f), 1f to Color.Transparent,
        center = Offset.Zero, radius = 1f,
    )

    init {
        for (i in 0 until r) {
            val a = Cage.rods[2 * i]; val b = Cage.rods[2 * i + 1]
            val mx = (Cage.nodes[3 * a] + Cage.nodes[3 * b]) / 2
            val my = (Cage.nodes[3 * a + 1] + Cage.nodes[3 * b + 1]) / 2
            val mz = (Cage.nodes[3 * a + 2] + Cage.nodes[3 * b + 2]) / 2
            for (l in 0 until 9) rodSpill[i * 9 + l] = falloff(mx, my, mz, l)
        }
        for (i in 0 until n) for (l in 0 until 9) {
            nodeSpill[i * 9 + l] = falloff(Cage.nodes[3 * i], Cage.nodes[3 * i + 1], Cage.nodes[3 * i + 2], l)
        }

        // Block faces, wound counter-clockwise as seen from outside. Corner bits: 1 = x max, 2 = y max, 4 = z max.
        val lo = Cage.coreMin; val hi = Cage.coreMax
        FACE.forEachIndexed { f, corners ->
            corners.forEachIndexed { c, bits ->
                quad[f * 12 + c * 3] = if (bits and 1 == 0) lo[0] else hi[0]
                quad[f * 12 + c * 3 + 1] = if (bits and 2 == 0) lo[1] else hi[1]
                quad[f * 12 + c * 3 + 2] = if (bits and 4 == 0) lo[2] else hi[2]
            }
        }
        flat(BOARD, -0.21f, -0.66f, 0.21f, -0.40f, 0.03f, 0f)
        flat(USB, -0.105f, -0.76f, 0.105f, -0.63f, 0.07f, 0f)
        for (l in 0 until 9) {
            val h = LED_HALF
            flat(LED0 + l, Cage.leds[3 * l] - h, Cage.leds[3 * l + 1] - h, Cage.leds[3 * l] + h, Cage.leds[3 * l + 1] + h, Cage.leds[3 * l + 2], LED_TURN[l])
        }

        // The data wire runs LED to LED in chain order, then down to the board.
        for (w in 0 until WIRES) {
            val a = w
            val bx: Float; val by: Float; val bz: Float
            if (w < 8) { bx = Cage.leds[3 * (w + 1)]; by = Cage.leds[3 * (w + 1) + 1]; bz = Cage.leds[3 * (w + 1) + 2] } else { bx = 0.12f; by = -0.52f; bz = 0.05f }
            val ax = if (w < 8) Cage.leds[3 * a] else Cage.leds[24]
            val ay = if (w < 8) Cage.leds[3 * a + 1] else Cage.leds[25]
            val az = if (w < 8) Cage.leds[3 * a + 2] else Cage.leds[26]
            val k = hash(w * 31 + 9) - 0.5f
            floatArrayOf(
                ax, ay, az - 0.025f,
                (ax + bx) / 2 * 0.9f + 0.05f * k, (ay + by) / 2 * 0.9f - 0.03f, min(az, bz) - 0.05f - 0.03f * hash(w + 1),
                bx, by, bz - 0.025f,
            ).copyInto(wire, w * 9)
        }

        for (i in 0 until OUTLINE) {
            val t = 2 * PI * i / OUTLINE
            val rad = Cage.surfaceRadius(cos(t), sin(t), 0.0)
            outline[2 * i] = ((cos(t) * rad - Cage.offset[0]) * Cage.scale[0]).toFloat()
            outline[2 * i + 1] = ((sin(t) * rad - Cage.offset[1]) * Cage.scale[1]).toFloat()
        }
    }

    // A quad facing +z at depth z, turned by [turn] radians about its centre, wound like the block's front face.
    private fun flat(q: Int, x0: Float, y0: Float, x1: Float, y1: Float, z: Float, turn: Float) {
        val cx = (x0 + x1) / 2; val cy = (y0 + y1) / 2
        val c = cos(turn); val s = sin(turn)
        val xs = floatArrayOf(x0, x0, x1, x1); val ys = floatArrayOf(y0, y1, y1, y0)
        for (k in 0..3) {
            val dx = xs[k] - cx; val dy = ys[k] - cy
            quad[q * 12 + k * 3] = cx + dx * c - dy * s
            quad[q * 12 + k * 3 + 1] = cy + dx * s + dy * c
            quad[q * 12 + k * 3 + 2] = z
        }
    }

    private fun falloff(x: Float, y: Float, z: Float, l: Int): Float {
        val dx = x - Cage.leds[3 * l]; val dy = y - Cage.leds[3 * l + 1]; val dz = z - Cage.leds[3 * l + 2]
        return 1f / (1f + 9f * (dx * dx + dy * dy + dz * dz))
    }

    private var cyw = 1f; private var syw = 0f; private var cp = 1f; private var sp = 0f; private var sq = 1f

    private fun view(x: Float, y: Float, z: Float) {
        val x1 = (x * cyw + z * syw) * sq
        val z1 = (-x * syw + z * cyw) * sq
        val y1 = y * sq
        tx = x1
        ty = y1 * cp - z1 * sp
        tz = y1 * sp + z1 * cp
    }

    private fun persp(z: Float) = CAM / (CAM - z)

    /**
     * One frame. [rgb] is the 9 LEDs' firmware colours (0..255, chain order), [yaw]/[pitch] in radians, [squash]
     * the tap pulse scale, [center] and [halfWidth] place the cage on the canvas.
     */
    fun DrawScope.draw(rgb: IntArray, yaw: Float, pitch: Float, squash: Float, look: Look, center: Offset, halfWidth: Float) {
        for (i in 0 until 27) light[i] = rgb[i] / 255f
        var total = 0f
        for (i in 0 until 27) total += light[i]
        total /= 9f // 0..3

        cyw = cos(yaw); syw = sin(yaw); cp = cos(pitch); sp = sin(pitch); sq = squash
        // On the pale cloth a fully dimmed beacon would go dark brown: asleep it dims part way, cools and thins into
        // the cloth; offline it pales and greys out.
        dim = when (look) { Look.AWAKE -> 1f; Look.ASLEEP -> if (paper) 0.66f else 0.5f; Look.OFFLINE -> if (paper) 0.8f else 0.44f }
        sat = if (look == Look.OFFLINE) 0.12f else 1f
        alpha = when { look == Look.OFFLINE -> 0.58f; look == Look.ASLEEP && paper -> 0.8f; else -> 1f }
        cool = look == Look.ASLEEP
        hw = halfWidth; cx0 = center.x; cy0 = center.y

        for (i in 0 until n) {
            view(Cage.nodes[3 * i], Cage.nodes[3 * i + 1], Cage.nodes[3 * i + 2])
            vx[i] = tx; vy[i] = ty; vz[i] = tz
            val p = persp(tz)
            sx[i] = cx0 + tx * hw * p
            sy[i] = cy0 - ty * hw * p
            ss[i] = p
            // Tin: grey, a little warm, lit from the upper left; the LEDs colour it.
            // The far joints fall back harder than the rods, so they do not read as a second layer.
            val depth = shadeOf(fog(tz).let { it * (0.5f + 0.5f * it) })
            var c = SOLDER * depth * dim
            var cr = c; var cg = c * 0.985f; var cb = c * 0.95f
            for (l in 0 until 9) {
                val s = nodeSpill[i * 9 + l] * 0.72f * dim
                cr += light[3 * l] * s; cg += light[3 * l + 1] * s; cb += light[3 * l + 2] * s
            }
            nr[i] = cr; ng[i] = cg; nb[i] = cb
        }
        for (k in 0 until Q * 4) {
            view(quad[k * 3], quad[k * 3 + 1], quad[k * 3 + 2])
            qvx[k] = tx; qvy[k] = ty; qvz[k] = tz
            val p = persp(tz)
            qsx[k] = cx0 + tx * hw * p
            qsy[k] = cy0 - ty * hw * p
        }
        for (w in 0 until WIRES) {
            var z = 0f
            for (k in 0..2) {
                view(wire[w * 9 + k * 3], wire[w * 9 + k * 3 + 1], wire[w * 9 + k * 3 + 2])
                val p = persp(tz)
                wsx[w * 3 + k] = cx0 + tx * hw * p
                wsy[w * 3 + k] = cy0 - ty * hw * p
                z += tz
            }
            wz[w] = z / 3
        }

        // The cloth under it: a contact shadow, then whatever the LEDs throw on it.
        val floorY = cy0 + hw * 1.03f
        withTransform({ translate(cx0, floorY); scale(hw * 1.0f, hw * 0.14f, Offset.Zero) }) {
            drawCircle(shade, 1f, Offset.Zero, alpha = alpha)
        }
        // A dark core where the tip touches, so it stands on the cloth.
        withTransform({ translate(cx0, floorY - hw * 0.02f); scale(hw * 0.34f, hw * 0.05f, Offset.Zero) }) {
            drawCircle(shade, 1f, Offset.Zero, alpha = alpha)
        }
        if (total > 0.01f) {
            val mean = ledMean()
            val k = if (paper) 0.9f else 1f
            glow(cx0, floorY, hw * 1.3f, hw * 0.24f, mean, (0.5f * total).coerceAtMost(0.7f) * dim * k)
            // Bloom in the air around the cage, the LEDs' colour filling the case.
            glow(cx0, cy0, hw * 1.8f, hw * 1.8f, mean, (0.30f * total).coerceAtMost(0.45f) * dim * k)
        }

        // Sort everything back to front: key = quantised depth << 16 | item.
        // A bead is drawn after every rod that ends in it, so the rods run into the tin and never across it.
        var k = 0
        for (i in 0 until n) front[i] = vz[i]
        for (i in 0 until r) {
            val a = Cage.rods[2 * i]; val b = Cage.rods[2 * i + 1]
            val z = (vz[a] + vz[b]) / 2
            if (z > front[a]) front[a] = z
            if (z > front[b]) front[b] = z
            order[k++] = key(z, i)
        }
        for (i in 0 until n) order[k++] = key(front[i] + 0.004f, r + i)
        // The LEDs sit on the block's front face, so they never sort behind it: by centres alone, a turned face's
        // centre passes the LEDs on its far side and hides them (one side of the ring went dark).
        val mount = quadDepth(FRONT)
        for (q in 0 until Q) {
            val z = when {
                q >= LED0 -> maxOf(quadDepth(q), mount) + 0.012f
                q >= BOARD -> quadDepth(q) + 0.004f
                else -> quadDepth(q)
            }
            order[k++] = key(z, r + n + q)
        }
        order[k++] = key(quadDepth(BACK) - 0.012f, r + n + Q)
        for (w in 0 until WIRES) order[k++] = key(wz[w], r + n + Q + 1 + w)
        order.sort()

        val rodW = ROD * hw
        for (o in order) {
            val item = o and 0xFFFF
            fade = 1f
            when {
                item < r -> rod(item, rodW)
                item < r + n -> node(item - r, rodW)
                item < r + n + FACES -> face(item - r - n, total)
                item == r + n + BOARD -> board()
                item == r + n + USB -> usb()
                item < r + n + Q -> led(item - r - n - LED0)
                item == r + n + Q -> button()
                else -> wire(item - r - n - Q - 1, rodW)
            }
        }
        fade = 1f
    }

    /**
     * The wave a sent mood leaves: the heart's own outline growing out of the cage and fading, [t] 0..1. Drawn
     * before the cage so the rods stay in front of it.
     */
    fun DrawScope.wave(t: Float, color: Color, yaw: Float, pitch: Float, center: Offset, halfWidth: Float) {
        cyw = cos(yaw); syw = sin(yaw); cp = cos(pitch); sp = sin(pitch); sq = 1f
        for (ring in 0..1) {
            val u = t * 1.25f - ring * 0.25f
            if (u <= 0f || u >= 1f) continue
            val e = 1f - (1f - u) * (1f - u) * (1f - u)
            val grow = 1.04f + 0.62f * e
            path.reset()
            for (i in 0 until OUTLINE) {
                view(outline[2 * i] * grow, (outline[2 * i + 1] + 0.03f) * grow - 0.03f, 0f)
                val p = persp(tz)
                val x = center.x + tx * halfWidth * p; val y = center.y - ty * halfWidth * p
                if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
            }
            path.close()
            val a = (1f - u) * (1f - u) * if (ring == 0) 1f else 0.6f
            drawPath(path, color.copy(alpha = a * 0.5f), style = Stroke(width = halfWidth * 0.05f * (1f - 0.6f * u)), blendMode = lightBlend)
            drawPath(path, color.copy(alpha = a), style = Stroke(width = halfWidth * 0.012f * (1f - 0.5f * u) + 1f), blendMode = lightBlend)
        }
    }

    /**
     * True on the pale daylight cloth: the far side fades into the cloth instead of sinking into the dark, and light
     * is laid over it instead of added (adding to a light ground only bleaches it).
     */
    var paper = false

    private val lightBlend get() = if (paper) BlendMode.SrcOver else BlendMode.Plus

    private var dim = 1f; private var sat = 1f; private var alpha = 1f; private var cool = false; private var fade = 1f
    private var hw = 1f; private var cx0 = 0f; private var cy0 = 0f

    private fun key(z: Float, item: Int) = (((z + 3f) * 5000f).toInt().coerceIn(0, 32767) shl 16) or item

    // Nearer is brighter and fuller; the far side of the cage sinks into the dark.
    private fun fog(z: Float) = 0.26f + 0.74f * smooth((z + 0.62f) / 1.2f)

    // How much of the fog darkens: all of it on felt; on the pale cloth only a little, the rest goes to [fadeOf].
    private fun shadeOf(f: Float) = if (paper) 0.74f + 0.26f * f else f
    private fun fadeOf(f: Float) = if (paper) 0.3f + 0.7f * f else 1f

    private fun DrawScope.rod(i: Int, w: Float) {
        val a = Cage.rods[2 * i]; val b = Cage.rods[2 * i + 1]
        var ux = vx[b] - vx[a]; var uy = vy[b] - vy[a]; var uz = vz[b] - vz[a]
        val len = sqrt(ux * ux + uy * uy + uz * uz)
        ux /= len; uy /= len; uz /= len
        // A thin cylinder under a warm key and a cool fill (Kajiya-Kay); the highlight is kept wide and soft.
        val tl = ux * LX + uy * LY + uz * LZ
        val th = ux * HX + uy * HY + uz * HZ
        val tf = ux * FX + uy * FY + uz * FZ
        val diff = 1f - tl * tl
        val spec = max(0f, 1f - th * th).pow(11)
        val fill = sqrt(max(0f, 1f - tf * tf))
        val zm = (vz[a] + vz[b]) / 2
        val far = fog(zm)
        val depth = shadeOf(far)
        fade = fadeOf(far)
        val tone = rodTone[i]
        val k = (0.13f + 0.50f * diff + 0.10f * fill) * depth * dim * tone
        val s = spec * 0.46f * depth * dim
        var cr = BRASS_R * k + s * 1.00f + fill * 0.012f
        var cg = BRASS_G * k + s * 0.84f + fill * 0.016f
        var cb = BRASS_B * k + s * 0.50f + fill * 0.024f
        for (l in 0 until 9) {
            val g = rodSpill[i * 9 + l] * 0.62f * dim
            cr += light[3 * l] * g; cg += light[3 * l + 1] * g; cb += light[3 * l + 2] * g
        }
        val ax = sx[a]; val ay = sy[a]; val bx = sx[b]; val by = sy[b]
        val width = w * (ss[a] + ss[b]) / 2
        if (far < 0.52f) {
            drawLine(color(cr * 0.82f, cg * 0.82f, cb * 0.82f), Offset(ax, ay), Offset(bx, by), strokeWidth = width * 0.9f, cap = StrokeCap.Round)
            return
        }
        // Dark flank, lit body, soft glint: three strokes read as a round rod.
        drawLine(color(cr * 0.36f, cg * 0.34f, cb * 0.32f), Offset(ax, ay), Offset(bx, by), strokeWidth = width, cap = StrokeCap.Round)
        drawLine(color(cr, cg, cb), Offset(ax, ay), Offset(bx, by), strokeWidth = width * 0.64f, cap = StrokeCap.Round)
        if (spec > 0.2f) {
            val g = 0.55f * spec
            drawLine(color(cr + g, cg + g * 0.9f, cb + g * 0.62f, 0.75f), Offset(ax, ay), Offset(bx, by), strokeWidth = width * 0.24f, cap = StrokeCap.Round)
        }
    }

    // A rounded bead of tin over the rod ends: one smooth dome, lit from the upper left, with a wet point of light.
    private fun DrawScope.node(i: Int, w: Float) {
        val rad = w * 1.42f * nodeSize[i] * ss[i]
        val x = sx[i]; val y = sy[i]
        fade = fadeOf(fog(vz[i]).let { it * (0.5f + 0.5f * it) })
        val cr = nr[i]; val cg = ng[i]; val cb = nb[i]
        withTransform({ translate(x, y); scale(rad, rad, Offset.Zero) }) {
            drawCircle(dome, 1f, Offset.Zero, colorFilter = ColorFilter.tint(color(cr * 1.22f, cg * 1.22f, cb * 1.22f), BlendMode.Modulate))
        }
        if (vz[i] < -0.05f) return
        drawCircle(color(cr * 1.5f + 0.2f, cg * 1.5f + 0.2f, cb * 1.5f + 0.2f, 0.85f), rad * 0.17f, Offset(x - rad * 0.30f, y - rad * 0.34f))
    }

    private fun quadDepth(q: Int): Float = (qvz[q * 4] + qvz[q * 4 + 1] + qvz[q * 4 + 2] + qvz[q * 4 + 3]) / 4f

    // Faces the camera when its projected corners keep their winding.
    private fun facing(q: Int): Boolean {
        val o = q * 4
        val ax = qsx[o + 1] - qsx[o]; val ay = qsy[o + 1] - qsy[o]
        val bx = qsx[o + 3] - qsx[o]; val by = qsy[o + 3] - qsy[o]
        return ax * by - ay * bx > 0f
    }

    private fun trace(q: Int, inset: Float = 0f) {
        val o = q * 4
        val mx = (qsx[o] + qsx[o + 1] + qsx[o + 2] + qsx[o + 3]) / 4
        val my = (qsy[o] + qsy[o + 1] + qsy[o + 2] + qsy[o + 3]) / 4
        path.reset()
        for (c in 0..3) {
            val x = qsx[o + c] + (mx - qsx[o + c]) * inset; val y = qsy[o + c] + (my - qsy[o + c]) * inset
            if (c == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        path.close()
    }

    // The battery block: black shrink-wrap with a satin sheen, picking up the LEDs on its front.
    private fun DrawScope.face(f: Int, total: Float) {
        if (!facing(f)) return
        val o = f * 4
        val ex = qvx[o + 1] - qvx[o]; val ey = qvy[o + 1] - qvy[o]; val ez = qvz[o + 1] - qvz[o]
        val gx = qvx[o + 3] - qvx[o]; val gy = qvy[o + 3] - qvy[o]; val gz = qvz[o + 3] - qvz[o]
        var wx = ey * gz - ez * gy; var wy = ez * gx - ex * gz; var wz0 = ex * gy - ey * gx
        val wl = sqrt(wx * wx + wy * wy + wz0 * wz0); wx /= wl; wy /= wl; wz0 /= wl
        val lit = max(0f, -(wx * LX + wy * LY + wz0 * LZ))
        val base = (0.030f + 0.055f * lit) * dim
        val glowK = if (f == FRONT) 0.11f * total * dim else 0.03f * total * dim
        val m = ledMean()
        trace(f)
        drawPath(path, color(base + m.red * glowK, base * 0.96f + m.green * glowK, base * 1.04f + m.blue * glowK))
        if (f == FRONT || f == BACK) {
            // Sheen: a soft band across the wrap, from its upper-left corner.
            drawPath(
                path,
                Brush.linearGradient(
                    0f to Color.Transparent, 0.3f to Color(1f, 1f, 1f, 0.075f * dim * alpha), 0.55f to Color.Transparent,
                    start = Offset(qsx[o + 1], qsy[o + 1]), end = Offset(qsx[o + 3], qsy[o + 3]),
                ),
            )
        }
    }

    // The ESP32 board under the battery: blue solder mask from the front, bare dark laminate from behind.
    private fun DrawScope.board() {
        val front = facing(BOARD)
        trace(BOARD)
        val d = dim * fog(quadDepth(BOARD))
        if (front) drawPath(path, color(0.035f * d, 0.07f * d, 0.17f * d)) else drawPath(path, color(0.035f * d, 0.05f * d, 0.07f * d))
        if (!front) return
        // The module's metal can.
        val o = BOARD * 4
        fun px(u: Float, v: Float) = (qsx[o] * (1 - u) + qsx[o + 3] * u) * (1 - v) + (qsx[o + 1] * (1 - u) + qsx[o + 2] * u) * v
        fun py(u: Float, v: Float) = (qsy[o] * (1 - u) + qsy[o + 3] * u) * (1 - v) + (qsy[o + 1] * (1 - u) + qsy[o + 2] * u) * v
        path.reset()
        path.moveTo(px(0.56f, 0.2f), py(0.56f, 0.2f)); path.lineTo(px(0.56f, 0.8f), py(0.56f, 0.8f))
        path.lineTo(px(0.9f, 0.8f), py(0.9f, 0.8f)); path.lineTo(px(0.9f, 0.2f), py(0.9f, 0.2f)); path.close()
        drawPath(path, color(0.30f * d, 0.31f * d, 0.32f * d))
        // A row of pads along the board's edge breaks the plain rectangle.
        for (k in 0 until 6) {
            val u = 0.1f + k * 0.07f
            drawLine(color(0.50f * d, 0.40f * d, 0.18f * d), Offset(px(u, 0.16f), py(u, 0.16f)), Offset(px(u, 0.36f), py(u, 0.36f)), strokeWidth = hw * 0.008f)
        }
    }

    private fun DrawScope.usb() {
        val front = facing(USB)
        val d = dim * fog(quadDepth(USB))
        trace(USB)
        drawPath(path, if (front) color(0.60f * d, 0.61f * d, 0.62f * d) else color(0.28f * d, 0.28f * d, 0.29f * d))
        if (!front) return
        // The socket's mouth: a rounded slot across the shell.
        val o = USB * 4
        val lx0 = qsx[o] * 0.5f + qsx[o + 1] * 0.5f; val ly0 = qsy[o] * 0.5f + qsy[o + 1] * 0.5f
        val rx0 = qsx[o + 3] * 0.5f + qsx[o + 2] * 0.5f; val ry0 = qsy[o + 3] * 0.5f + qsy[o + 2] * 0.5f
        val tall = sqrt((qsx[o + 1] - qsx[o]) * (qsx[o + 1] - qsx[o]) + (qsy[o + 1] - qsy[o]) * (qsy[o + 1] - qsy[o]))
        drawLine(
            color(0.06f * d, 0.06f * d, 0.07f * d),
            Offset(lx0 + (rx0 - lx0) * 0.2f, ly0 + (ry0 - ly0) * 0.2f), Offset(lx0 + (rx0 - lx0) * 0.8f, ly0 + (ry0 - ly0) * 0.8f),
            strokeWidth = tall * 0.42f, cap = StrokeCap.Round,
        )
    }

    private fun DrawScope.button() {
        if (!facing(BACK)) return
        val o = BACK * 4
        // The cap centre as a bilinear point on the projected back face (corners 0, 1, 3, 2 by bit order).
        val u = (Cage.button[0] - Cage.coreMin[0]) / (Cage.coreMax[0] - Cage.coreMin[0])
        val v = (Cage.button[1] - Cage.coreMin[1]) / (Cage.coreMax[1] - Cage.coreMin[1])
        // BACK is wound (0, 1, 3, 2): o = (xmin, ymin), o+1 = (xmax, ymin), o+2 = (xmax, ymax), o+3 = (xmin, ymax).
        val x = (qsx[o] * (1 - u) + qsx[o + 1] * u) * (1 - v) + (qsx[o + 3] * (1 - u) + qsx[o + 2] * u) * v
        val y = (qsy[o] * (1 - u) + qsy[o + 1] * u) * (1 - v) + (qsy[o + 3] * (1 - u) + qsy[o + 2] * u) * v
        val ax = qsx[o + 1] - qsx[o]; val ay = qsy[o + 1] - qsy[o]
        val rad = Cage.BUTTON_R * hw * (sqrt(ax * ax + ay * ay) / ((Cage.coreMax[0] - Cage.coreMin[0]) * hw)).coerceAtLeast(0.2f)
        drawCircle(color(0.05f * dim, 0.05f * dim, 0.05f * dim), rad * 1.6f, Offset(x, y))
        drawCircle(color(0.62f * dim, 0.33f * dim, 0.08f * dim), rad, Offset(x, y))
        drawCircle(color(0.88f * dim, 0.53f * dim, 0.16f * dim), rad * 0.82f, Offset(x - rad * 0.06f, y - rad * 0.08f))
        drawCircle(color(1f * dim, 0.76f * dim, 0.42f * dim, 0.7f), rad * 0.36f, Offset(x - rad * 0.26f, y - rad * 0.3f))
    }

    // Enamelled copper, one hair thick.
    private fun DrawScope.wire(w: Int, rodW: Float) {
        val d = shadeOf(fog(wz[w])) * dim
        fade = fadeOf(fog(wz[w]))
        path.reset()
        path.moveTo(wsx[w * 3], wsy[w * 3])
        path.quadraticTo(wsx[w * 3 + 1], wsy[w * 3 + 1], wsx[w * 3 + 2], wsy[w * 3 + 2])
        drawPath(path, color(0.62f * d, 0.31f * d, 0.14f * d), style = Stroke(width = rodW * 0.42f, cap = StrokeCap.Round))
    }

    // A 5050 package: white body, round lens, and its light.
    private fun DrawScope.led(l: Int) {
        val q = LED0 + l
        val o = q * 4
        val x = (qsx[o] + qsx[o + 2]) / 2; val y = (qsy[o] + qsy[o + 2]) / 2
        val r0 = light[3 * l]; val g0 = light[3 * l + 1]; val b0 = light[3 * l + 2]
        val on = max(r0, max(g0, b0))
        val front = facing(q)
        val p = persp(quadDepth(q))
        trace(q)
        if (front) {
            drawPath(path, color(0.84f * dim + r0 * 0.35f, 0.83f * dim + g0 * 0.35f, 0.79f * dim + b0 * 0.35f))
            // Lens: an ellipse on the package's own axes, so it foreshortens with it.
            val ux = (qsx[o + 3] - qsx[o]) * 0.36f; val uy = (qsy[o + 3] - qsy[o]) * 0.36f
            val wx = (qsx[o + 1] - qsx[o]) * 0.36f; val wy = (qsy[o + 1] - qsy[o]) * 0.36f
            path.reset()
            for (k in 0 until 14) {
                val t = (2 * PI * k / 14).toFloat()
                val px = x + cos(t) * ux + sin(t) * wx; val py = y + cos(t) * uy + sin(t) * wy
                if (k == 0) path.moveTo(px, py) else path.lineTo(px, py)
            }
            path.close()
            // Lit, the lens is the LED's own colour, not white: the phosphor's yellow gives way to it.
            val off = 1f - on
            drawPath(path, color(0.69f * dim * off + r0, 0.66f * dim * off + g0, 0.55f * dim * off + b0))
        } else {
            drawPath(path, color(0.10f * dim, 0.11f * dim, 0.10f * dim))
        }
        if (on > 0.004f) {
            val vis = if (front) 1f else 0.4f
            // The hue at full strength; how bright it is goes into the alpha.
            val c = Color((r0 / on).coerceAtMost(1f), (g0 / on).coerceAtMost(1f), (b0 / on).coerceAtMost(1f))
            val wide = hw * p * (0.30f + 0.46f * on)
            glow(x, y, wide, wide, c, (0.8f * on * vis * dim).coerceAtMost(1f))
            val tight = hw * p * 0.15f
            glow(x, y, tight, tight, c, (on * vis * dim).coerceAtMost(1f))
            // A hot point, the colour washed toward white, as a lit LED reads to the eye.
            if (front) {
                val hot = Color(0.55f + 0.45f * c.red, 0.55f + 0.45f * c.green, 0.55f + 0.45f * c.blue)
                glow(x, y, hw * p * 0.06f, hw * p * 0.06f, hot, (0.9f * on * dim).coerceAtMost(1f))
            }
        }
    }

    private fun DrawScope.glow(x: Float, y: Float, rx: Float, ry: Float, c: Color, a: Float) {
        withTransform({ translate(x, y); scale(rx, ry, Offset.Zero) }) {
            drawCircle(glow, 1f, Offset.Zero, alpha = a * alpha, colorFilter = ColorFilter.tint(tint(c), BlendMode.Modulate), blendMode = lightBlend)
        }
    }

    private fun ledMean(): Color {
        var r0 = 0f; var g0 = 0f; var b0 = 0f
        for (l in 0 until 9) { r0 += light[3 * l]; g0 += light[3 * l + 1]; b0 += light[3 * l + 2] }
        val m = max(r0, max(g0, max(b0, 1e-3f)))
        return Color(r0 / m, g0 / m, b0 / m)
    }

    private fun tint(c: Color) = color(c.red, c.green, c.blue, 1f, solid = true)

    private fun color(r0: Float, g0: Float, b0: Float, a: Float = 1f, solid: Boolean = false): Color {
        var cr = r0; var cg = g0; var cb = b0
        if (cool) { cr *= 0.80f; cg *= 0.92f; cb *= 1.12f }
        val lum = 0.3f * cr + 0.59f * cg + 0.11f * cb
        return Color(
            (lum + (cr - lum) * sat).coerceIn(0f, 1f),
            (lum + (cg - lum) * sat).coerceIn(0f, 1f),
            (lum + (cb - lum) * sat).coerceIn(0f, 1f),
            if (solid) 1f else a * alpha * fade,
        )
    }

    private fun smooth(t: Float): Float { val c = t.coerceIn(0f, 1f); return c * c * (3 - 2 * c) }

    // A fixed scatter in 0..1 for hand-made variation; the same every run.
    private fun hash(i: Int): Float {
        var h = i * 374761393 + 668265263
        h = (h xor (h ushr 13)) * 1274126177
        return ((h xor (h ushr 16)) and 0xFFFF) / 65535f
    }

    internal companion object {
        const val CAM = 5.2f
        const val FACES = 6
        const val BACK = 4
        const val FRONT = 5
        const val BOARD = 6
        const val USB = 7
        const val LED0 = 8
        const val Q = LED0 + 9
        const val WIRES = 9
        const val OUTLINE = 72
        const val ROD = 0.0205f
        const val LED_HALF = 0.056f

        // How each LED sits, radians: along the ring (its neighbours' chord, mod 90°), so pixel i and 9 - i mirror.
        val LED_TURN = floatArrayOf(0f, -0.71f, 0.26f, -0.67f, 0.11f, -0.11f, 0.67f, -0.26f, 0.71f)

        // Key light (upper left, in front), its half vector with the view direction (0, 0, 1), and a cool fill from
        // the right and behind.
        const val LX = -0.45f; const val LY = 0.62f; const val LZ = 0.64f
        val HX: Float; val HY: Float; val HZ: Float
        const val FX = 0.74f; const val FY = 0.30f; const val FZ = -0.60f
        init {
            val hx = LX; val hy = LY; val hz = LZ + 1f
            val hl = sqrt(hx * hx + hy * hy + hz * hz)
            HX = hx / hl; HY = hy / hl; HZ = hz / hl
        }
        const val BRASS_R = 0.86f; const val BRASS_G = 0.63f; const val BRASS_B = 0.25f
        const val SOLDER = 0.80f

        // Corner bits: 1 = x max, 2 = y max, 4 = z max.
        val FACE = arrayOf(
            intArrayOf(0, 2, 6, 4), // x min
            intArrayOf(1, 5, 7, 3), // x max
            intArrayOf(0, 4, 5, 1), // y min
            intArrayOf(2, 3, 7, 6), // y max
            intArrayOf(0, 1, 3, 2), // z min (back, button side)
            intArrayOf(4, 6, 7, 5), // z max (front)
        )
    }
}
