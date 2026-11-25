"""Save library (launcher/save_tools.py): New Save, Save Current, Use, Rename, Delete."""
import importlib.util
import tempfile
import time
import unittest
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "save_tools", Path(__file__).resolve().parents[1] / "launcher" / "save_tools.py")
st = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(st)


class SaveSlots(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.game = st.save_dir(self.work)
        self.game.mkdir(parents=True)
        (self.game / "main").write_bytes(b"v1")

    def tearDown(self):
        self.tmp.cleanup()

    def play(self, data: bytes):           # the game writes a newer save
        (self.game / "main").write_bytes(data)

    def test_new_save_names_and_copies(self):
        a = st.new_slot(self.work)
        b = st.new_slot(self.work)
        self.assertEqual((a["name"], b["name"]), ("Save 1", "Save 2"))
        self.assertEqual((a["path"] / "data" / "main").read_bytes(), b"v1")
        self.assertEqual(st.active_slot(self.work), b["id"])
        self.assertEqual([s["name"] for s in st.list_slots(self.work)], ["Save 1", "Save 2"])

    def test_new_save_without_game_save(self):
        (self.game / "main").unlink()
        with self.assertRaises(ValueError):
            st.new_slot(self.work)

    def test_save_current_updates_in_place_with_backup(self):
        a = st.new_slot(self.work)
        created = st.list_slots(self.work)[0]["created"]
        self.play(b"v2")
        time.sleep(0.01)
        old = st.save_current(self.work, a["id"])
        slots = st.list_slots(self.work)
        self.assertEqual(len(slots), 1)                                  # no new save
        self.assertEqual((a["path"] / "data" / "main").read_bytes(), b"v2")
        self.assertEqual((old / "main").read_bytes(), b"v1")             # previous copy kept
        self.assertTrue(old.is_relative_to(st.backup_dir(self.work)))
        self.assertEqual(slots[0]["created"], created)
        self.assertGreater(slots[0]["updated"], created)

    def test_use_slot_backs_up_game_save(self):
        a = st.new_slot(self.work)
        self.play(b"other")
        st.new_slot(self.work)
        old = st.load_slot(self.work, a["id"])
        self.assertEqual((self.game / "main").read_bytes(), b"v1")
        self.assertEqual((old / "main").read_bytes(), b"other")
        self.assertEqual(st.active_slot(self.work), a["id"])

    def test_rename(self):
        a = st.new_slot(self.work)
        st.rename_slot(self.work, a["id"], "  Before Volo  ")
        self.assertEqual(st.list_slots(self.work)[0]["name"], "Before Volo")
        with self.assertRaises(ValueError):
            st.rename_slot(self.work, a["id"], "   ")

    def test_delete_moves_to_backups_and_never_touches_game_save(self):
        a = st.new_slot(self.work)
        b = st.new_slot(self.work)
        dst = st.delete_slot(self.work, b["id"])
        self.assertEqual([s["id"] for s in st.list_slots(self.work)], [a["id"]])
        self.assertTrue((dst / "data" / "main").is_file())               # recoverable
        self.assertEqual((self.game / "main").read_bytes(), b"v1")       # game save untouched
        self.assertIsNone(st.active_slot(self.work))
        with self.assertRaises(ValueError):
            st.delete_slot(self.work, b["id"])                           # already gone
        with self.assertRaises(ValueError):
            st.delete_slot(self.work, 99)

    def test_only_real_slot_folders(self):
        (st.slots_dir(self.work) / "slot_7").mkdir(parents=True)           # no slot.json: not a save
        self.assertEqual(st.list_slots(self.work), [])
        with self.assertRaises(ValueError):
            st.delete_slot(self.work, 7)
        self.assertTrue((st.slots_dir(self.work) / "slot_7").is_dir())


if __name__ == "__main__":
    unittest.main()
