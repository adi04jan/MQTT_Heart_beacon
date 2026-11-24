package com.heartbeacon.app.ui.heart

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.scale
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/** How the beacon is: lit and normal, asleep (dimmed, cool), or unreachable (a desaturated ghost). */
enum class Look { AWAKE, ASLEEP, OFFLINE }

/**
 * Draws [Cage] as the real object: brass rods shaded as thin cylinders (Kajiya-Kay), silver solder nodes, the dark
 * core block with its orange button on the back, and the 9 LEDs with their light falling on the rods and the floor.
 * Everything is depth-sorted back to front each frame; nothing is allocated per frame except the few gradient
 * brushes. Only draws when asked, so an idle beacon costs nothing.
 */
class CageRenderer {
    private val n = Cage.nodeCount
    private val r = Cage.rodCount

    // Static LED light falloff on every rod midpoint and node: 1 / (1 + 9 d^2) in model space.
    private val rodSpill = FloatArray(r * 9)
    private val nodeSpill = FloatArray(n * 9)

    // Per frame, view space and screen space.
    private val vx = FloatArray(n); private val vy = FloatArray(n); private val vz = FloatArray(n)
    private val sx = FloatArray(n); private val sy = FloatArray(n); private val ss = FloatArray(n)
    private val lx = FloatArray(9); private val ly = FloatArray(9); private val lz = FloatArray(9); private val ls = FloatArray(9)
    private val cx8 = FloatArray(8); private val cy8 = FloatArray(8); private val cz8 = FloatArray(8)
    private val px8 = FloatArray(8); private val py8 = FloatArray(8)
    private val order = IntArray(r + n + FACES + 1 + 9)
    private val light = FloatArray(27)
    private val path = Path()

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
    }

    private fun falloff(x: Float, y: Float, z: Float, l: Int): Float {
        val dx = x - Cage.leds[3 * l]; val dy = y - Cage.leds[3 * l + 1]; val dz = z - Cage.leds[3 * l + 2]
        return 1f / (1f + 9f * (dx * dx + dy * dy + dz * dz))
    }

    /**
     * One frame. [rgb] is the 9 LEDs' firmware colours (0..255, chain order), [yaw]/[pitch] in radians, [squash]
     * the tap pulse scale, [center] and [halfWidth] place the cage on the canvas.
     */
    fun DrawScope.draw(rgb: IntArray, yaw: Float, pitch: Float, squash: Float, look: Look, center: Offset, halfWidth: Float) {
        for (i in 0 until 27) light[i] = rgb[i] / 255f
        var total = 0f
        for (i in 0 until 27) total += light[i]
        total /= 9f // 0..3

        val cyw = cos(yaw); val syw = sin(yaw); val cp = cos(pitch); val sp = sin(pitch)
        ledNormalZ = cyw * cp // the LED ring faces model +z
        fun view(x: Float, y: Float, z: Float, out: Int, ox: FloatArray, oy: FloatArray, oz: FloatArray) {
            val x1 = (x * cyw + z * syw) * squash
            val z1 = (-x * syw + z * cyw) * squash
            val y1 = y * squash
            ox[out] = x1
            oy[out] = y1 * cp - z1 * sp
            oz[out] = y1 * sp + z1 * cp
        }
        fun persp(z: Float) = CAM / (CAM - z)

        for (i in 0 until n) {
            view(Cage.nodes[3 * i], Cage.nodes[3 * i + 1], Cage.nodes[3 * i + 2], i, vx, vy, vz)
            val p = persp(vz[i])
            sx[i] = center.x + vx[i] * halfWidth * p
            sy[i] = center.y - vy[i] * halfWidth * p
            ss[i] = p
        }
        for (l in 0 until 9) {
            view(Cage.leds[3 * l], Cage.leds[3 * l + 1], Cage.leds[3 * l + 2], l, lx, ly, lz)
            ls[l] = persp(lz[l])
        }
        for (c in 0 until 8) {
            view(
                if (c and 1 == 0) Cage.coreMin[0] else Cage.coreMax[0],
                if (c and 2 == 0) Cage.coreMin[1] else Cage.coreMax[1],
                if (c and 4 == 0) Cage.coreMin[2] else Cage.coreMax[2],
                c, cx8, cy8, cz8,
            )
            val p = persp(cz8[c])
            px8[c] = center.x + cx8[c] * halfWidth * p
            py8[c] = center.y - cy8[c] * halfWidth * p
        }

        val dim = when (look) { Look.AWAKE -> 1f; Look.ASLEEP -> 0.5f; Look.OFFLINE -> 0.42f }
        val sat = if (look == Look.OFFLINE) 0.15f else 1f
        val alpha = if (look == Look.OFFLINE) 0.6f else 1f

        // Floor: the cage's soft pool of light, warmed by whatever the LEDs show (a circle squashed into an ellipse,
        // so the falloff stays smooth all round).
        val floor = Offset(center.x, center.y + halfWidth * 1.1f)
        scale(1f, 0.16f, pivot = floor) {
            drawCircle(
                Brush.radialGradient(listOf(Color(0.16f, 0.13f, 0.12f, 0.7f * alpha), Color.Transparent), center = floor, radius = halfWidth * 1.15f),
                radius = halfWidth * 1.15f, center = floor,
            )
        }
        if (total > 0.01f) {
            val (r0, g0, b0) = ledMean()
            scale(1f, 0.16f, pivot = floor) {
                drawCircle(
                    Brush.radialGradient(
                        listOf(Color(r0, g0, b0, (0.4f * total).coerceAtMost(0.55f) * dim), Color.Transparent),
                        center = floor, radius = halfWidth * 0.95f,
                    ),
                    radius = halfWidth * 0.95f, center = floor, blendMode = BlendMode.Plus,
                )
            }
            // Bloom behind the cage.
            drawCircle(
                Brush.radialGradient(
                    listOf(Color(r0, g0, b0, (0.22f * total).coerceAtMost(0.35f) * dim), Color.Transparent),
                    center = center, radius = halfWidth * 1.35f,
                ),
                radius = halfWidth * 1.35f, center = center, blendMode = BlendMode.Plus,
            )
        }

        // Sort everything back to front: key = quantised depth << 16 | item.
        var k = 0
        for (i in 0 until r) {
            val a = Cage.rods[2 * i]; val b = Cage.rods[2 * i + 1]
            order[k++] = key((vz[a] + vz[b]) / 2, i)
        }
        for (i in 0 until n) order[k++] = key(vz[i] + 0.004f, r + i)
        for (f in 0 until FACES) order[k++] = key(faceDepth(f), r + n + f)
        order[k++] = key(buttonDepth(), r + n + FACES)
        for (l in 0 until 9) order[k++] = key(lz[l] + 0.01f, r + n + FACES + 1 + l)
        order.sort()

        val rodW = 0.019f * halfWidth
        for (o in order) {
            val item = o and 0xFFFF
            when {
                item < r -> rod(item, rodW, dim, sat, alpha)
                item < r + n -> node(item - r, rodW, dim, sat, alpha)
                item < r + n + FACES -> face(item - r - n, dim, alpha, total)
                item == r + n + FACES -> button(center, halfWidth, dim, sat, alpha)
                else -> led(item - r - n - FACES - 1, center, halfWidth, dim, sat, alpha)
            }
        }
    }

    private fun key(z: Float, item: Int) = (((z + 3f) * 5000f).toInt().coerceIn(0, 32767) shl 16) or item

    private fun DrawScope.rod(i: Int, w: Float, dim: Float, sat: Float, alpha: Float) {
        val a = Cage.rods[2 * i]; val b = Cage.rods[2 * i + 1]
        var tx = vx[b] - vx[a]; var ty = vy[b] - vy[a]; var tz = vz[b] - vz[a]
        val len = sqrt(tx * tx + ty * ty + tz * tz)
        tx /= len; ty /= len; tz /= len
        val tl = tx * LX + ty * LY + tz * LZ
        val th = tx * HX + ty * HY + tz * HZ
        val diff = sqrt(max(0f, 1f - tl * tl))
        val spec = sqrt(max(0f, 1f - th * th)).pow(90)
        val zm = (vz[a] + vz[b]) / 2
        val depth = 0.36f + 0.64f * smooth((zm + 0.6f) / 1.2f)
        val shade = (0.16f + 0.84f * diff * diff) * depth * dim
        var cr = BRASS_R * shade + spec * 0.55f * depth * dim
        var cg = BRASS_G * shade + spec * 0.50f * depth * dim
        var cb = BRASS_B * shade + spec * 0.34f * depth * dim
        for (l in 0 until 9) {
            val s = rodSpill[i * 9 + l] * 1.25f * dim
            cr += light[3 * l] * s; cg += light[3 * l + 1] * s; cb += light[3 * l + 2] * s
        }
        val c = color(cr, cg, cb, sat, alpha)
        drawLine(c, Offset(sx[a], sy[a]), Offset(sx[b], sy[b]), strokeWidth = w * (ss[a] + ss[b]) / 2, cap = StrokeCap.Round)
    }

    private fun DrawScope.node(i: Int, w: Float, dim: Float, sat: Float, alpha: Float) {
        val depth = 0.36f + 0.64f * smooth((vz[i] + 0.6f) / 1.2f)
        val rad = w * 1.25f * ss[i]
        var cr = SOLDER * 0.62f * depth * dim; var cg = cr; var cb = cr * 0.97f
        for (l in 0 until 9) {
            val s = nodeSpill[i * 9 + l] * 1.1f * dim
            cr += light[3 * l] * s; cg += light[3 * l + 1] * s; cb += light[3 * l + 2] * s
        }
        val o = Offset(sx[i], sy[i])
        drawCircle(color(cr, cg, cb, sat, alpha), rad, o)
        drawCircle(color(0.95f * depth * dim, 0.95f * depth * dim, 0.92f * depth * dim, sat, alpha * 0.85f), rad * 0.38f, Offset(o.x - rad * 0.32f, o.y - rad * 0.34f))
    }

    // Box faces as corner indices (bit0 = x max, bit1 = y max, bit2 = z max) and their model normals.
    private fun faceDepth(f: Int): Float { var z = 0f; for (c in FACE[f]) z += cz8[c]; return z / 4f }

    private fun DrawScope.face(f: Int, dim: Float, alpha: Float, total: Float) {
        val q = FACE[f]
        // Visible when its view normal faces the camera: cross product of two edges in screen space.
        val ax = px8[q[1]] - px8[q[0]]; val ay = py8[q[1]] - py8[q[0]]
        val bx = px8[q[3]] - px8[q[0]]; val by = py8[q[3]] - py8[q[0]]
        if (ax * by - ay * bx <= 0f) return
        val nx = cx8[q[1]] - cx8[q[0]]; val ny = cy8[q[1]] - cy8[q[0]]; val nz = cz8[q[1]] - cz8[q[0]]
        val mx = cx8[q[3]] - cx8[q[0]]; val my = cy8[q[3]] - cy8[q[0]]; val mz = cz8[q[3]] - cz8[q[0]]
        var wx = ny * mz - nz * my; var wy = nz * mx - nx * mz; var wz = nx * my - ny * mx
        val wl = sqrt(wx * wx + wy * wy + wz * wz); wx /= wl; wy /= wl; wz /= wl
        val lit = max(0f, -(wx * LX + wy * LY + wz * LZ))
        val base = (0.035f + 0.06f * lit) * dim
        val glow = if (f == FRONT) 0.10f * total * dim else 0f
        val (gr, gg, gb) = ledMean()
        path.reset()
        path.moveTo(px8[q[0]], py8[q[0]])
        for (c in 1..3) path.lineTo(px8[q[c]], py8[q[c]])
        path.close()
        drawPath(path, Color((base + gr * glow).coerceIn(0f, 1f), (base * 0.94f + gg * glow).coerceIn(0f, 1f), (base * 1.05f + gb * glow).coerceIn(0f, 1f), alpha))
    }

    private fun buttonDepth(): Float = faceDepth(BACK) + 0.01f

    private fun DrawScope.button(center: Offset, hw: Float, dim: Float, sat: Float, alpha: Float) {
        val q = FACE[BACK]
        val ax = px8[q[1]] - px8[q[0]]; val ay = py8[q[1]] - py8[q[0]]
        val bx = px8[q[3]] - px8[q[0]]; val by = py8[q[3]] - py8[q[0]]
        if (ax * by - ay * bx <= 0f) return
        // The cap centre as a bilinear point on the projected back face.
        val u = (Cage.button[0] - Cage.coreMin[0]) / (Cage.coreMax[0] - Cage.coreMin[0])
        val v = (Cage.button[1] - Cage.coreMin[1]) / (Cage.coreMax[1] - Cage.coreMin[1])
        val x = bilerp(px8, u, v); val y = bilerp(py8, u, v)
        val rad = Cage.BUTTON_R * hw * (sqrt(ax * ax + ay * ay) / ((Cage.coreMax[0] - Cage.coreMin[0]) * hw)).coerceAtLeast(0.2f)
        drawCircle(color(0.06f * dim, 0.06f * dim, 0.06f * dim, sat, alpha), rad * 1.55f, Offset(x, y))
        drawCircle(color(0.86f * dim, 0.52f * dim, 0.16f * dim, sat, alpha), rad, Offset(x, y))
        drawCircle(color(1f * dim, 0.78f * dim, 0.45f * dim, sat, alpha * 0.7f), rad * 0.4f, Offset(x - rad * 0.3f, y - rad * 0.3f))
    }

    // Bilinear point on the back face (corners z=min: 0 = (xmin,ymin), 1 = (xmax,ymin), 2 = (xmin,ymax), 3 = (xmax,ymax)).
    private fun bilerp(a: FloatArray, u: Float, v: Float) =
        (a[0] * (1 - u) + a[1] * u) * (1 - v) + (a[2] * (1 - u) + a[3] * u) * v

    private fun DrawScope.led(l: Int, center: Offset, hw: Float, dim: Float, sat: Float, alpha: Float) {
        val p = ls[l]
        val x = center.x + lx[l] * hw * p
        val y = center.y - ly[l] * hw * p
        val nz = ledNormalZ
        val r0 = light[3 * l]; val g0 = light[3 * l + 1]; val b0 = light[3 * l + 2]
        val on = max(r0, max(g0, b0))
        val half = 0.05f * hw * p
        val sq = max(0.15f, nz) // foreshortened square
        if (nz > 0f) {
            drawRect(
                color(0.86f * dim + r0 * 0.4f, 0.85f * dim + g0 * 0.4f, 0.80f * dim + b0 * 0.4f, sat, alpha),
                topLeft = Offset(x - half * sq, y - half),
                size = androidx.compose.ui.geometry.Size(2 * half * sq, 2 * half),
            )
            val lens = Color(
                (0.62f * dim * (1 - on) + r0).coerceAtMost(1f),
                (0.58f * dim * (1 - on) + g0).coerceAtMost(1f),
                (0.42f * dim * (1 - on) + b0).coerceAtMost(1f),
                alpha,
            )
            drawCircle(if (sat < 1f) color(lens.red, lens.green, lens.blue, sat, alpha) else lens, half * 0.62f * sq.coerceAtLeast(0.6f), Offset(x, y))
        }
        if (on > 0.004f) {
            val vis = if (nz > 0f) 1f else 0.35f
            val glowR = hw * p * (0.22f + 0.28f * on)
            drawCircle(
                Brush.radialGradient(
                    listOf(Color(r0.coerceAtMost(1f), g0.coerceAtMost(1f), b0.coerceAtMost(1f), 0.95f * on * vis * dim), Color.Transparent),
                    center = Offset(x, y), radius = glowR,
                ),
                radius = glowR, center = Offset(x, y), blendMode = BlendMode.Plus,
            )
            if (nz > 0f) drawCircle(Color(1f, 1f, 1f, 0.85f * on * dim), half * 0.28f, Offset(x, y), blendMode = BlendMode.Plus)
        }
    }

    // View z of the LED ring's normal this frame (the same for all nine).
    private var ledNormalZ = 1f

    private fun ledMean(): Triple<Float, Float, Float> {
        var r0 = 0f; var g0 = 0f; var b0 = 0f
        for (l in 0 until 9) { r0 += light[3 * l]; g0 += light[3 * l + 1]; b0 += light[3 * l + 2] }
        val m = max(r0, max(g0, max(b0, 1e-3f)))
        return Triple(r0 / m, g0 / m, b0 / m)
    }

    private fun color(r0: Float, g0: Float, b0: Float, sat: Float, a: Float): Color {
        val lum = 0.3f * r0 + 0.59f * g0 + 0.11f * b0
        return Color(
            (lum + (r0 - lum) * sat).coerceIn(0f, 1f),
            (lum + (g0 - lum) * sat).coerceIn(0f, 1f),
            (lum + (b0 - lum) * sat).coerceIn(0f, 1f),
            a,
        )
    }

    private fun smooth(t: Float): Float { val c = t.coerceIn(0f, 1f); return c * c * (3 - 2 * c) }

    private companion object {
        const val CAM = 5.2f
        const val FACES = 6
        const val FRONT = 5
        const val BACK = 4
        // Key light (upper left, in front) and the half vector with the view direction (0, 0, 1).
        val LX = -0.45f; val LY = 0.62f; val LZ = 0.64f
        val HX: Float; val HY: Float; val HZ: Float
        init {
            val hx = LX; val hy = LY; val hz = LZ + 1f
            val hl = sqrt(hx * hx + hy * hy + hz * hz)
            HX = hx / hl; HY = hy / hl; HZ = hz / hl
        }
        const val BRASS_R = 0.66f; const val BRASS_G = 0.50f; const val BRASS_B = 0.21f
        const val SOLDER = 0.80f

        // Quads wound counter-clockwise as seen from outside. Corner bits: 1 = x max, 2 = y max, 4 = z max.
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
