"""Keep the first-start wiring on the same sign-in UI as Settings.

Behavioral outcomes are tested with auth/API doubles in ChatGPTSetupTests.swift
and web/tests/chatGPTSetup.tsx. These checks cover the native sheet's composition
without launching the app or accessing the user's Keychain.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_mac_first_start_and_settings_share_the_sign_in_controls():
    settings = (ROOT / "macOS/Sources/App/Settings/ChatGPTSettings.swift").read_text()
    setup = (ROOT / "macOS/Sources/App/Guide/GuideViews.swift").read_text().split("struct GuideSetupView", 1)[1]
    assert 'ChatGPTConnection(model: connection)' in settings
    assert 'ChatGPTConnection(model: chatGPT, onboarding: true)' in setup
    assert 'Button("Continue with ChatGPT", action: action)' in settings
    assert 'try await GuideController.shared.finishChatGPTSetup()' in setup
    assert 'Button("スキップして始める")' in setup
    assert 'SecureField(' in setup
    assert '.signIn(' not in setup.split('/// A floating panel', 1)[0]
