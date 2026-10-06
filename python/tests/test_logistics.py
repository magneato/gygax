import os
import tempfile
import unittest

from gygax import LocalService, ServiceError


MODEL_SKU = "ba99x"
MODEL_RANGE_METERS = 20_000
FLOW_CRUISE_SPEED_METERS_PER_SECOND = 12
PLANNER_CRUISE_SPEED_METERS_PER_SECOND = 10
DRONE_KIND = "quadcopter"
POWER_SOURCE = "solar"
LEDGER_FILENAME = "ledger.jsonl"
LOST_UNIT_STATUS = "lost"

ADDED_UNIT_COUNT = 8
REMOVED_UNIT_COUNT = 3
OVER_REMOVE_UNIT_COUNT = 99
EXPECTED_ACTIVE_UNIT_COUNT = ADDED_UNIT_COUNT - REMOVED_UNIT_COUNT
SUMMARY_WINDOW = "1d"
HTTP_CONFLICT_STATUS = 409

ORIGIN_LATITUDE = 0.0
ORIGIN_LONGITUDE = 0.0
DESTINATION_LONGITUDE = 0.3
REFUEL_SITE_ID = "mid"
REFUEL_SITE_LONGITUDE = 0.15
REFUEL_SERVICES = [POWER_SOURCE]
REFUEL_STOP_COUNT = 1
REFUEL_STOP_TYPE = "refuel"
WAYPOINT_STOP_TYPE = "waypoint"


@unittest.skipUnless(os.environ.get("GYGAX_BIN"), "GYGAX_BIN not set")
class LogisticsTests(unittest.TestCase):
    def test_flow_summary_and_persistence(self):
        with tempfile.TemporaryDirectory() as tmp:
            journal = os.path.join(tmp, LEDGER_FILENAME)
            with LocalService(ledger=journal) as svc:
                logistics = svc.client.logistics
                logistics.put_model(
                    MODEL_SKU,
                    max_range_m=MODEL_RANGE_METERS,
                    cruise_mps=FLOW_CRUISE_SPEED_METERS_PER_SECOND,
                    kind=DRONE_KIND,
                    power=POWER_SOURCE,
                )
                add_record = f"+{ADDED_UNIT_COUNT} {MODEL_SKU} drone={DRONE_KIND} power={POWER_SOURCE}"
                made = logistics.record(add_record)
                self.assertEqual(len(made["guids"]), ADDED_UNIT_COUNT)
                remove_record = f"-{REMOVED_UNIT_COUNT} {MODEL_SKU} drone={DRONE_KIND} power={POWER_SOURCE}"
                lost = logistics.record(remove_record)
                self.assertEqual(lost["line"], remove_record)
                with self.assertRaises(ServiceError) as ctx:
                    logistics.record(f"-{OVER_REMOVE_UNIT_COUNT} {MODEL_SKU}")
                self.assertEqual(ctx.exception.status, HTTP_CONFLICT_STATUS)
                summary = logistics.summary(SUMMARY_WINDOW)
                self.assertEqual(summary["totals"]["active"], EXPECTED_ACTIVE_UNIT_COUNT)
                self.assertEqual(summary["totals"]["lost"], REMOVED_UNIT_COUNT)
                self.assertEqual(
                    {flow["line"] for flow in summary["flow"]},
                    {add_record, remove_record},
                )
            with LocalService(ledger=journal) as svc:
                self.assertEqual(
                    svc.client.logistics.summary(SUMMARY_WINDOW)["totals"]["active"],
                    EXPECTED_ACTIVE_UNIT_COUNT,
                )
                self.assertEqual(
                    len(svc.client.logistics.units(status=LOST_UNIT_STATUS)),
                    REMOVED_UNIT_COUNT,
                )

    def test_planner_inserts_refuel_stop_and_suggests_one_when_blocked(self):
        with LocalService() as svc:
            logistics = svc.client.logistics
            logistics.put_model(
                MODEL_SKU,
                max_range_m=MODEL_RANGE_METERS,
                cruise_mps=PLANNER_CRUISE_SPEED_METERS_PER_SECOND,
                power=POWER_SOURCE,
            )
            request = dict(
                sku=MODEL_SKU,
                origin={"latitude": ORIGIN_LATITUDE, "longitude": ORIGIN_LONGITUDE},
                waypoints=[
                    {"latitude": ORIGIN_LATITUDE, "longitude": DESTINATION_LONGITUDE}
                ],
            )
            blocked = logistics.plan(**request)
            self.assertFalse(blocked["feasible"])
            point = blocked["suggestion"]["add_refuel_point"]
            self.assertGreater(point["longitude"], ORIGIN_LONGITUDE)
            logistics.put_site(
                REFUEL_SITE_ID,
                ORIGIN_LATITUDE,
                REFUEL_SITE_LONGITUDE,
                services=REFUEL_SERVICES,
            )
            plan = logistics.plan(**request)
            self.assertTrue(plan["feasible"])
            self.assertEqual(plan["refuel_stops"], REFUEL_STOP_COUNT)
            self.assertEqual(
                [stop["type"] for stop in plan["route"]],
                [REFUEL_STOP_TYPE, WAYPOINT_STOP_TYPE],
            )


if __name__ == "__main__":
    unittest.main()
