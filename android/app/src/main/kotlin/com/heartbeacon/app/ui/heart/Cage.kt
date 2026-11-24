package com.heartbeacon.app.ui.heart

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * The beacon as built: a geodesic cage of brass rods (frequency-3 icosphere, 92 solder nodes, 270 rods) pushed out
 * onto a heart surface, around a dark battery-and-board block, with the 9 LEDs in a heart ring facing the front.
 * Proportions come from the photos of the real one (front width : height : depth = 1 : 0.97 : 0.5).
 *
 * Model space: x right, y up, z toward the viewer; the cage spans x -1..1. Pure math, no Android, host-tested.
 */
object Cage {
    /** Node positions, xyz interleaved. */
    val nodes: FloatArray

    /** Rods as node index pairs. */
    val rods: IntArray

    val nodeCount get() = nodes.size / 3
    val rodCount get() = rods.size / 2

    /**
     * LED centres in chain order (pixel 0 first), xyz interleaved, from the front photo.
     * ponytail: chain order is a guess (bottom tip, then clockwise up the left lobe); confirm with the battery bar
     * (hold the button: pixel 0 lights first) and reorder here.
     */
    val leds = floatArrayOf(
        0.02f, -0.42f, 0.12f,
        -0.31f, -0.24f, 0.12f,
        -0.60f, 0.05f, 0.12f,
        -0.54f, 0.47f, 0.12f,
        -0.20f, 0.58f, 0.12f,
        0.20f, 0.62f, 0.12f,
        0.49f, 0.57f, 0.12f,
        0.60f, 0.15f, 0.12f,
        0.32f, -0.19f, 0.12f,
    )

    /** The dark core block (battery + boards): min and max corners. */
    val coreMin = floatArrayOf(-0.36f, -0.47f, -0.22f)
    val coreMax = floatArrayOf(0.40f, 0.62f, 0.10f)

    /** Orange button cap on the block's back face: centre and radius. */
    val button = floatArrayOf(0.02f, 0.24f, -0.235f)
    const val BUTTON_R = 0.075f

    // Heart frame scaling found by [build]: implicit-surface coordinates = model / scale + offset.
    internal val scale = FloatArray(3)
    internal val offset = FloatArray(3)

    /**
     * The classic implicit heart (x width, u up, d depth): negative inside, zero on the surface.
     * (x² + 9/4 d² + u² - 1)³ - x² u³ - 9/80 d² u³
     */
    fun implicit(x: Double, u: Double, d: Double): Double {
        val a = x * x + 2.25 * d * d + u * u - 1.0
        return a * a * a - x * x * u * u * u - 0.1125 * d * d * u * u * u
    }

    /**
     * The cage's shape: [implicit] with the top notch deepened (the real cage's dip between the lobes is sharper than
     * the formula's), by sampling higher up near the centre line.
     */
    fun shape(x: Double, u: Double, d: Double): Double = implicit(x, u + NOTCH * kotlin.math.exp(-(x * x) / 0.09) * maxOf(0.0, u), d)

    private const val NOTCH = 0.22

    /** Distance from the origin to the cage surface along unit direction (x, u, d): first sign change, bisected. */
    fun surfaceRadius(x: Double, u: Double, d: Double): Double {
        var lo = 0.0
        var hi = 0.02
        while (shape(x * hi, u * hi, d * hi) < 0) {
            lo = hi
            hi += 0.02
        }
        repeat(40) {
            val m = (lo + hi) / 2
            if (shape(x * m, u * m, d * m) < 0) lo = m else hi = m
        }
        return (lo + hi) / 2
    }

    init {
        val (dirs, edges) = geodesic(3)
        // Directions: icosphere z is the depth axis (a node faces the viewer, as on the real cage).
        val raw = DoubleArray(dirs.size)
        for (i in 0 until dirs.size / 3) {
            val x = dirs[3 * i]
            val u = dirs[3 * i + 1]
            val d = dirs[3 * i + 2]
            val r = surfaceRadius(x, u, d)
            raw[3 * i] = x * r
            raw[3 * i + 1] = u * r
            raw[3 * i + 2] = d * r
        }
        val min = DoubleArray(3) { Double.MAX_VALUE }
        val max = DoubleArray(3) { -Double.MAX_VALUE }
        for (i in raw.indices) {
            min[i % 3] = minOf(min[i % 3], raw[i])
            max[i % 3] = maxOf(max[i % 3], raw[i])
        }
        // Centred on its box; x spans -1..1, height 0.97 of the width, depth half the width (the real cage).
        val size = doubleArrayOf(2.0, 2.0 * 0.97, 1.0)
        nodes = FloatArray(raw.size)
        for (k in 0..2) {
            scale[k] = (size[k] / (max[k] - min[k])).toFloat()
            offset[k] = ((min[k] + max[k]) / 2).toFloat()
        }
        for (i in raw.indices) nodes[i] = ((raw[i] - offset[i % 3]) * scale[i % 3]).toFloat()
        rods = edges
    }

    /** Model point -> implicit-surface coordinates (for tests: is it inside the cage?). */
    fun toImplicit(x: Float, y: Float, z: Float) = doubleArrayOf(
        (x / scale[0] + offset[0]).toDouble(),
        (y / scale[1] + offset[1]).toDouble(),
        (z / scale[2] + offset[2]).toDouble(),
    )

    /** Unit-sphere geodesic of frequency [f]: (directions xyz, edge pairs), shared vertices merged. */
    internal fun geodesic(f: Int): Pair<DoubleArray, IntArray> {
        val h = 1 / sqrt(5.0)
        val s = 2 / sqrt(5.0)
        val ico = ArrayList<DoubleArray>()
        ico += doubleArrayOf(0.0, 0.0, 1.0)
        for (k in 0..4) ico += doubleArrayOf(s * sin(2 * PI * k / 5), s * cos(2 * PI * k / 5), h)
        for (k in 0..4) ico += doubleArrayOf(s * sin(2 * PI * k / 5 + PI / 5), s * cos(2 * PI * k / 5 + PI / 5), -h)
        ico += doubleArrayOf(0.0, 0.0, -1.0)
        val faces = ArrayList<IntArray>()
        for (k in 0..4) {
            val u0 = 1 + k
            val u1 = 1 + (k + 1) % 5
            val l0 = 6 + k
            val l1 = 6 + (k + 1) % 5
            faces += intArrayOf(0, u0, u1)
            faces += intArrayOf(u0, l0, u1)
            faces += intArrayOf(u1, l0, l1)
            faces += intArrayOf(11, l1, l0)
        }
        val verts = ArrayList<DoubleArray>()
        val index = HashMap<Triple<Long, Long, Long>, Int>()
        fun vertex(p: DoubleArray): Int {
            val n = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2])
            val q = doubleArrayOf(p[0] / n, p[1] / n, p[2] / n)
            val key = Triple((q[0] * 1e6).toLong(), (q[1] * 1e6).toLong(), (q[2] * 1e6).toLong())
            return index.getOrPut(key) { verts += q; verts.size - 1 }
        }
        val edges = HashSet<Long>()
        fun edge(a: Int, b: Int) {
            edges += minOf(a, b).toLong() shl 32 or maxOf(a, b).toLong()
        }
        for (fc in faces) {
            val (a, b, c) = Triple(ico[fc[0]], ico[fc[1]], ico[fc[2]])
            val grid = Array(f + 1) { IntArray(f + 1) }
            for (i in 0..f) for (j in 0..f - i) {
                grid[i][j] = vertex(DoubleArray(3) { k -> a[k] + (b[k] - a[k]) * i / f + (c[k] - a[k]) * j / f })
            }
            for (i in 0 until f) for (j in 0 until f - i) {
                edge(grid[i][j], grid[i + 1][j])
                edge(grid[i][j], grid[i][j + 1])
                edge(grid[i + 1][j], grid[i][j + 1])
            }
        }
        val out = DoubleArray(verts.size * 3)
        verts.forEachIndexed { i, v -> v.copyInto(out, 3 * i) }
        val pairs = IntArray(edges.size * 2)
        edges.sorted().forEachIndexed { i, e ->
            pairs[2 * i] = (e shr 32).toInt()
            pairs[2 * i + 1] = (e and 0xFFFFFFFFL).toInt()
        }
        return out to pairs
    }
}
