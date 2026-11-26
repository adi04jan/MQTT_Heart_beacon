package com.heartbeacon.app.data

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.Base64

class WireTest {
    private fun obj(s: String) = Json.parseToJsonElement(s).jsonObject

    @Test fun eventsReadAsYouOrTheSenderWithMarksOnYoursOnly() {
        val rows = Json.parseToJsonElement("""[
            {"id":"a","k":"heart","ts":100,"uid":"me","from":"Me","src":"me","to":[{"did":"HB-000001","st":"shown"},{"did":"HB-000002","st":"pending"}]},
            {"id":"b","k":"sorry","ts":90,"uid":"asha","from":"Asha","src":"HB-3FA2C1","to":[{"did":"HB-000001","st":"shown"}]},
            {"id":"c","k":"annoy","ts":80,"uid":"me","from":"Me","src":"HB-000001","to":[{"did":"HB-000002","st":"expired"}]},
            {"id":"d","k":"heart","ts":70,"uid":"me","from":"Me","src":"me","to":[]},
            {"id":"e","k":"wave","ts":60,"uid":"asha","from":"Asha","src":"asha","to":[]}
        ]""") as JsonArray
        val ev = eventsOf(rows, "me")
        assertEquals(listOf("a", "b", "c", "d"), ev.map { it.id }) // unknown kinds are skipped
        assertEquals(Event("a", Mood.HEART, null, 100_000, Mark.SHOWN), ev[0])
        assertEquals(Event("b", Mood.SORRY, "Asha", 90_000, null), ev[1])
        assertEquals(Mark.EXPIRED, ev[2].mark) // pressed on my own beacon: still mine
        assertNull(ev[3].mark) // nobody else's beacon to reach
    }

    @Test fun markPrefersShownThenAllExpired() {
        assertEquals(Mark.QUEUED, markOf(listOf("pending", "sent")))
        assertEquals(Mark.QUEUED, markOf(listOf("expired", "sent")))
        assertEquals(Mark.SHOWN, markOf(listOf("expired", "shown")))
        assertEquals(Mark.EXPIRED, markOf(listOf("expired", "expired")))
    }

    @Test fun beaconCardFromMe() {
        val b = beaconOf(obj("""{"did":"HB-3FA2C1","name":"","cfg_n":3,"st":"sleep","wake":1795000000,"last_seen":1794990000,
            "tele":{"pct":82,"fw":"2.0.0"}}"""))
        assertEquals(Beacon("HB-3FA2C1", "HB-3FA2C1", Presence.Asleep(1_795_000_000_000), 82, 1_794_990_000_000), b)
        assertEquals(Presence.Offline, beaconOf(obj("""{"did":"HB-3FA2C1","name":"Desk"}""")).presence) // never seen
    }

    @Test fun settingsRoundTripKeepsNapMinutesAndLeavesTzToTheServer() {
        val s = obj("""{"n":4,"name":"Desk","led":{"bright":40},"net":{"use_wifi":false},"pwr":{"idle_nap_min":120},
            "sched":{"days":"1111110","on":true,"start":"09:00","stop":"19:00","tz":330}}""")
        val d = settingsOf(s, fw = "2.0.0", wifi = listOf("Office"), fwTarget = "2.0.1")
        assertEquals(DeviceSettings("Desk", listOf("Office"), false, true, "1111110", "09:00", "19:00", 40, true, "2.0.0", "2.0.1"), d)
        assertNull(settingsOf(s, "2.0.1", emptyList(), "2.0.1").fwAvailable) // already running it
        val body = settingsBody(d.copy(bright = 70), napMinOf(s), was = d)  // unchanged new keys stay out
        assertEquals(obj("""{"name":"Desk","led":{"bright":70},"sched":{"on":true,"days":"1111110","start":"09:00","stop":"19:00"},
            "pwr":{"idle_nap_min":120},"net":{"use_wifi":false}}"""), body)
        assertEquals(JsonPrimitive(90), settingsBody(d, 0)["pwr"]!!.jsonObject["idle_nap_min"])
        val breathe = settingsBody(d.copy(moodPat = 1), 0, was = d)  // a changed new key does go
        assertEquals(JsonPrimitive(1), breathe["mood"]!!.jsonObject["pat"])
        assertEquals(JsonPrimitive(false), breathe["led"]!!.jsonObject["presence"])
        assertEquals(JsonPrimitive(0), settingsBody(d.copy(napWhenIdle = false), 120)["pwr"]!!.jsonObject["idle_nap_min"])
    }

    @Test fun firmwareTargetComesFromTheSignedConfigBytes() {
        val b = Base64.getUrlEncoder().withoutPadding().encodeToString("""{"cv":1,"n":5,"fw":{"ver":"2.0.1","path":"/fw/hb-2.0.1.bin"}}""".encodeToByteArray())
        val cfg = buildJsonObject { put("n", JsonPrimitive(5)); put("b", JsonPrimitive(b)); put("s", JsonPrimitive("sig")); put("id", JsonPrimitive("x")) }
        assertEquals("2.0.1", fwTargetOf(cfg))
        assertEquals(setOf("n", "b", "s"), cfgBody(cfg).keys)
        assertNull(fwTargetOf(JsonObject(emptyMap())))
    }

    @Test fun beaconListsReadTolerantly() {
        assertEquals(listOf(WifiNet("Office", -48, true), WifiNet("Guest", -70, false)),
            netsOf(obj("""{"nets":[{"ssid":"Office","rssi":-48,"sec":true},{"ssid":"Guest","rssi":-70,"sec":false},{"rssi":-1}]}""")))
        assertEquals(listOf("Office", "Home"), ssidsOf(obj("""{"ssids":["Office","Home"]}""")))
        assertEquals(emptyList<WifiNet>(), netsOf(null))
    }
}
