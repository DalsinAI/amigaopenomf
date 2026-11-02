import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class StandaloneDependencyContractTests(unittest.TestCase):
    def test_catalog_pins_every_source_consumed_by_build(self):
        catalog = json.loads(
            (ROOT / "gameports/catalog.json").read_text(encoding="utf-8")
        )
        self.assertEqual(set(catalog["ports"]), {"openomf", "cdogs-sdl"})
        self.assertEqual(
            set(catalog["buildDependencies"]), {"amicradle-guest"}
        )
        for source in (
            *(port["source"] for port in catalog["ports"].values()),
            *catalog["buildDependencies"].values(),
        ):
            self.assertEqual(source["kind"], "git")
            self.assertRegex(source["commit"], r"^[0-9a-f]{40}$")

    def test_build_defaults_match_repository_local_preparation_layout(self):
        build = (ROOT / "build-amigaos3.sh").read_text(encoding="utf-8")
        self.assertIn('build/game-ports/sources/openomf', build)
        self.assertIn('build/game-ports/sources/cdogs-sdl', build)
        self.assertIn(
            'build/game-ports/dependencies/amicradle-guest', build
        )
        self.assertNotIn('build/game-ports/port-layer', build)


if __name__ == "__main__":
    unittest.main()
