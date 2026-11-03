#!/usr/bin/env python3
"""vzadg9r0 — GPU serialization contract: no per-frame full drains.

Red-first: the swapchain present path drained the GPU every frame
(`vkDeviceWaitIdle` before upload plus a fence wait after present), holding
the battle scene at ~20fps while neither CPU thread saturated. The contract
pins the fixed ordering:

* no `vkDeviceWaitIdle` inside the per-frame swapchain-draw path;
* no fence wait after `vkQueuePresentKHR` (single-flight: the next frame's
  pre-submit wait reaps the previous submit, letting game CPU overlap GPU);
* every frame still waits its fence BEFORE touching reusable resources
  (retired-texture reap, eviction, draw-buffer regrow), so destruction is
  covered by the fence instead of the device-wide drain.

Headless/readback paths are out of scope: they must wait for the copy.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BACKEND = REPO / "runtime" / "ge" / "vulkan_backend.cpp"


def read(path):
    return path.read_text(encoding="utf-8")


def function_body(text, name):
    start = text.find(name)
    if start < 0:
        return ""
    brace = text.find("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                body = text[start:i]
                return re.sub(r"//[^\n]*", "", body)
    return ""


class TestGpuSync(unittest.TestCase):
    def setUp(self):
        self.text = read(BACKEND)
        self.draws = function_body(self.text, "VulkanBackend::render_swapchain_draws")
        self.present = function_body(self.text, "bool VulkanBackend::render_swapchain(")
        self.assertTrue(self.draws, "render_swapchain_draws not found")
        self.assertTrue(self.present, "render_swapchain not found")

    def test_no_device_wait_idle_in_swapchain_draws(self):
        self.assertNotIn("vkDeviceWaitIdle", self.draws,
                         "per-frame full drain still present in swapchain draws")

    def test_no_fence_wait_after_present(self):
        for label, body in (("draws", self.draws), ("present", self.present)):
            tail = body.split("vkQueuePresentKHR")[-1]
            self.assertNotIn("vkWaitForFences", tail,
                             f"post-present fence wait still drains {label} path")

    def test_fence_wait_covers_resource_reuse(self):
        wait = self.draws.find("vkWaitForFences")
        self.assertGreaterEqual(wait, 0, "pre-submit fence wait missing")
        for token in ("reap_retired_textures", "evict_textures", "ensure_draw_buffers",
                      "create_draw_resources"):
            pos = self.draws.find(token)
            self.assertGreaterEqual(pos, 0, f"{token} missing from draws path")
            self.assertGreater(pos, wait,
                               f"{token} runs before the fence wait: use-after-submit hazard")


if __name__ == "__main__":
    unittest.main()
