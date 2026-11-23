"""Worker scratch cleanup across calls."""

from harness.real_cards import log
from projects import card, run_and_wait, write_nix_project as write
from test_worker_loading import native_card
from Tsugumi.Data.RunState import RunState


def test_mode_zero_scratch_does_not_fail_a_successful_call(core_factory, tmp_path):
    """A mode-000 nested directory is cleared between calls."""
    definition = native_card(r'''
        const auto temp = fs::path(std::getenv("TMPDIR")) / "private";
        fs::create_directories(temp / "nested");
        std::ofstream(temp / "nested/data") << "private";
        if (chmod((temp / "nested").c_str(), 0) || chmod(temp.c_str(), 0)) return 1;
        out.number(1);
    ''', reuseWorker=True, recordIO=True)
    write(tmp_path / "p", {"work": definition}, [card("work", "work")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(2):
        run_id, st = run_and_wait(c, "work")
        assert st.state == RunState.succeeded, log(c, run_id, "work", 2)
