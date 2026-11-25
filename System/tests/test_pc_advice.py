"""pc_advice: recommended settings per PC and the graphics card / balanced / processor workload choice."""
import importlib.util
from pathlib import Path

# loaded by path: putting launcher/ on sys.path would shadow the `launcher` package other tests import
_spec = importlib.util.spec_from_file_location("pc_advice", Path(__file__).resolve().parents[1] / "launcher" / "pc_advice.py")
pa = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pa)

LAPTOP = {"cpu": "Intel Core i5-8250U", "threads": 8, "ram_gb": 8.0, "gpu": "Intel(R) UHD Graphics 620", "vram_gb": 0.1}
MID = {"cpu": "Ryzen 5 3600", "threads": 12, "ram_gb": 16.0, "gpu": "NVIDIA GeForce GTX 1660 SUPER", "vram_gb": 6.0}
HIGH = {"cpu": "Ryzen 7 5700X", "threads": 16, "ram_gb": 32.0, "gpu": "NVIDIA GeForce RTX 5070", "vram_gb": 11.9}
OLD_CPU_BIG_GPU = {"cpu": "Core i5-4590", "threads": 4, "ram_gb": 16.0, "gpu": "AMD Radeon RX 7800 XT", "vram_gb": 16.0}


def test_tiers():
    assert pa.gpu_tier(LAPTOP) == 0 and pa.gpu_tier(MID) == 1 and pa.gpu_tier(HIGH) == 3
    assert pa.cpu_tier(OLD_CPU_BIG_GPU) == 0 and pa.cpu_tier(HIGH) == 3
    assert pa.has_dlss(HIGH) and not pa.has_dlss(MID) and not pa.has_dlss(OLD_CPU_BIG_GPU)


def test_recommendations():
    assert pa.recommend_workload(HIGH) == "Graphics card"          # processor-bound game: spare GPU time
    assert pa.recommend_workload(OLD_CPU_BIG_GPU) == "Graphics card"
    assert pa.recommend_workload(LAPTOP) == "Processor"            # integrated graphics: keep the picture light
    s = pa.settings_for(LAPTOP)
    assert s["res_scale"] == "Original" and s["dlaa"] == "Off" and s["fps"] == "60"
    assert s["draw_distance"] in ("Original", "High (2x)")         # 8 GB RAM: distances capped


def test_workloads_move_the_work():
    gpu, cpu = pa.settings_for(HIGH, "Graphics card"), pa.settings_for(HIGH, "Processor")
    assert gpu["npc_distance"] == "Original (100 m)" and gpu["res_scale"] != "Original" and gpu["dlaa"] == "On"
    assert cpu["npc_distance"] != "Original (100 m)" and cpu["res_scale"] == "Original" and cpu["dlaa"] == "Off"
    for hw in (LAPTOP, MID, HIGH, OLD_CPU_BIG_GPU):
        for w in pa.WORKLOADS:
            s = pa.settings_for(hw, w)
            assert s["res_scale"] in pa.RES and s["npc_distance"] in pa.NPC and s["shadow_quality"] in pa.SHADOW
            assert s["draw_distance"] in pa.DIST and s["fps"] in ("30", "60", "90", "120", "240")


def test_text_and_detect():
    assert pa.advice_text(HIGH).startswith("Recommended: 90 fps")
    hw = pa.detect()
    assert hw["threads"] >= 1 and isinstance(hw["gpu"], str)
