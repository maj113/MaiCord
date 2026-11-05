#!/usr/bin/env python3
"""A screen reader's view of the running Linux client, through the real AT-SPI registry (tools/test_atspi.sh).

Run against a MaiCord client on the signed-out login screen, on a private session bus where at-spi-bus-launcher serves
org.a11y.Bus with accessibility still off. It checks:

  * nothing is on the accessibility bus while org.a11y.Status says no assistive technology is on (the client costs
    nothing when unused);
  * turning IsEnabled on makes the application appear in the registry, with its window;
  * the login form's tree: roles, names, states, bounds and actions;
  * invoking a button's action, reading and setting a field's text, a password field that discloses nothing;
  * focus, text and window events arrive;
  * turning accessibility off takes the application off the bus again.

Needs python3-gi and gir1.2-atspi-2.0. Prints every result; exits nonzero when one failed.
"""

import sys
import time
import warnings

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib, Gio  # noqa: E402

warnings.simplefilter("ignore", DeprecationWarning)

failures = 0


def check(condition, label):
    global failures
    print(("ok   " if condition else "FAIL ") + label, flush=True)
    if not condition:
        failures += 1
    return bool(condition)


def pump(seconds):
    """Runs the GLib main context (libatspi's connection and its events) for a while."""
    end = time.monotonic() + seconds
    context = GLib.MainContext.default()
    while time.monotonic() < end:
        while context.iteration(False):
            pass
        time.sleep(0.01)


def wait_for(predicate, seconds, step=0.1):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        pump(step)
        value = predicate()
        if value:
            return value
    return predicate()


def children(accessible):
    try:
        return [accessible.get_child_at_index(i) for i in range(accessible.get_child_count())]
    except GLib.Error:
        return []


def walk(accessible, depth=0):
    yield accessible, depth
    for child in children(accessible):
        if child is not None:
            yield from walk(child, depth + 1)


def role(accessible):
    return accessible.get_role_name()


def find(root, name=None, role_name=None):
    for node, _ in walk(root):
        try:
            if (name is None or node.get_name() == name) and (role_name is None or role(node) == role_name):
                return node
        except GLib.Error:
            continue
    return None


def states(accessible):
    state_set = accessible.get_state_set()
    return {Atspi.StateType(i).value_nick for i in range(0, 44) if state_set.contains(Atspi.StateType(i))}


def find_application(name="MaiCord"):
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        try:
            application = desktop.get_child_at_index(i)
            if application is not None and application.get_name() == name:
                return application
        except GLib.Error:
            continue
    return None


def set_status(**flags):
    """What the desktop's accessibility switch (or a screen reader starting) does: org.a11y.Status properties."""
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    for name, value in flags.items():
        bus.call_sync("org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
                      GLib.Variant("(ssv)", ("org.a11y.Status", name, GLib.Variant("b", value))), None,
                      Gio.DBusCallFlags.NONE, 5000, None)


def dump(root):
    for node, depth in walk(root):
        try:
            print("  " * depth + "%s %r [%s]" % (role(node), node.get_name(), ",".join(sorted(states(node)))), flush=True)
        except GLib.Error:
            pass


def actions(accessible):
    try:
        return [Atspi.Action.get_action_name(accessible, i) for i in range(Atspi.Action.get_n_actions(accessible))]
    except GLib.Error:
        return []


def text_of(accessible):
    return Atspi.Text.get_text(accessible, 0, -1)


class Events:
    """The events a listener received; sources are named when asked, not inside the callback."""

    def __init__(self, kinds):
        self.items = []
        self.listener = Atspi.EventListener.new(self.on_event)
        for kind in kinds:
            self.listener.register(kind)

    def on_event(self, event):
        self.items.append((event.type, event.source, event.detail1, event.detail2, event.any_data))

    def clear(self):
        del self.items[:]

    def matching(self, prefix, name=None, detail1=None):
        found = []
        for kind, source, d1, d2, data in list(self.items):
            if not kind.startswith(prefix) or (detail1 is not None and d1 != detail1):
                continue
            try:
                if name is not None and source.get_name() != name:
                    continue
            except GLib.Error:
                continue
            found.append((kind, source, d1, d2, data))
        return found


def main():
    Atspi.init()
    # 1. Accessibility is off: the client is not on the accessibility bus.
    pump(1.0)
    check(find_application() is None, "the application is invisible while accessibility is off")

    # 2. A screen reader starts.
    set_status(IsEnabled=True)
    application = wait_for(find_application, 15)
    if not check(application is not None, "the application appears in the registry once accessibility is on"):
        return
    check(role(application) == "application", "the application's role is application")
    frames = children(application)
    check(len(frames) == 1 and role(frames[0]) == "frame" and frames[0].get_name() == "MaiCord",
          "the application has one child, the frame named MaiCord")
    window = frames[0]
    window_states = states(window)
    check({"showing", "enabled", "active"} <= window_states, "the window is showing, enabled and active")
    check(window.get_parent() is not None and role(window.get_parent()) == "application",
          "the window's parent is the application")

    # The signed-out UI.
    pump(1.0)
    print("--- the accessibility tree", flush=True)
    dump(window)
    print("---", flush=True)
    names = [(role(node), node.get_name()) for node, _ in walk(window)]

    card = find(window, "Log in", "dialog")
    check(card is not None, "the login card is a dialog named Log in")
    email = find(window, "Email or phone number", "entry")
    password = find(window, "Password", "password text")
    check(email is not None, "the identifier field is an entry named Email or phone number")
    check(password is not None, "the password field is a password text named Password")
    check(find(window, "Log in to MaiCord", "heading") is not None, "the form has its heading")
    buttons = [name for r, name in names if r == "push button"]
    check({"Forgot your password?", "Use browser", "Log in", "Minimize", "Close"} <= set(buttons),
          "the form's and the title bar's buttons are push buttons (%s)" % ", ".join(map(repr, buttons)))
    qr = find(window, "Log in with QR code", "image")
    check(qr is not None, "the QR code is an image named Log in with QR code")
    if qr is not None:
        check("showing" in states(qr), "the QR code is showing")
    if email is None or password is None or card is None:
        return

    # States and bounds.
    email_states = states(email)
    check({"editable", "enabled", "focusable", "single-line"} <= email_states, "the email field is editable, enabled and focusable")
    check("editable" in states(password), "the password field is editable")
    box = email.get_extents(Atspi.CoordType.WINDOW)
    window_box = window.get_extents(Atspi.CoordType.WINDOW)
    check(box.width > 50 and box.height > 10 and box.x >= 0 and box.y > 0,
          "the email field has bounds inside the window (%d,%d %dx%d)" % (box.x, box.y, box.width, box.height))
    check(window_box.x == 0 and window_box.y == 0 and window_box.width >= box.x + box.width and
          window_box.height >= box.y + box.height, "the field lies within the window's extents (%dx%d)" % (window_box.width, window_box.height))
    hit = Atspi.Component.get_accessible_at_point(window, box.x + box.width // 2, box.y + box.height // 2, Atspi.CoordType.WINDOW)
    check(hit is not None and hit.get_name() == "Email or phone number", "a hit test at the field's centre finds the field")
    parent_box = email.get_extents(Atspi.CoordType.PARENT)
    card_box = card.get_extents(Atspi.CoordType.WINDOW)
    check(parent_box.x == box.x - card_box.x and parent_box.y == box.y - card_box.y,
          "parent-relative extents are window extents minus the parent's")
    check(email.get_index_in_parent() >= 0 and email.get_parent().get_name() == "Log in", "the field knows its parent and index")

    # Events: register before acting.
    events = Events(["object:state-changed:focused", "object:text-changed:insert", "object:text-changed:delete",
                     "object:text-caret-moved", "window:activate", "object:children-changed",
                     "object:property-change:accessible-name", "object:announcement"])
    pump(0.3)

    # Focus: grab the password field, then the email field; the events follow.
    check(Atspi.Component.grab_focus(password), "Component.GrabFocus on the password field is accepted")
    check(bool(wait_for(lambda: events.matching("object:state-changed:focused", "Password", 1), 5)),
          "a focus event arrives for the password field")
    check("focused" in states(password), "the password field now reports focused")
    events.clear()
    check(Atspi.Component.grab_focus(email), "Component.GrabFocus on the email field is accepted")
    check(bool(wait_for(lambda: events.matching("object:state-changed:focused", "Email or phone number", 1), 5)),
          "a focus event arrives for the email field")
    check(bool(events.matching("object:state-changed:focused", "Password", 0)), "the password field's focus is reported lost")

    # Text: read and set.
    check(Atspi.Text.get_character_count(email) == 0, "the email field starts empty")
    events.clear()
    check(Atspi.EditableText.set_text_contents(email, "mai@example.com"), "EditableText.SetTextContents is accepted")
    check(wait_for(lambda: text_of(email) == "mai@example.com", 5),
          "the field's text reads back as what was set (%r)" % text_of(email))
    check(Atspi.Text.get_character_count(email) == 15, "the character count follows")
    inserted = wait_for(lambda: events.matching("object:text-changed:insert", "Email or phone number"), 5)
    check(bool(inserted) and inserted[0][4] == "mai@example.com" and inserted[0][3] == 15,
          "a text-inserted event carries the new text")
    check(wait_for(lambda: Atspi.Text.get_caret_offset(email) == 15, 3),
          "the caret is at the end of the text (%d)" % Atspi.Text.get_caret_offset(email))
    check(bool(events.matching("object:text-caret-moved", "Email or phone number", 15)),
          "a caret-moved event reports the caret at the end")
    word = Atspi.Text.get_string_at_offset(email, 5, Atspi.TextGranularity.WORD)
    check(word.content == "example" and word.start_offset == 4 and word.end_offset == 11,
          "word granularity answers the word around an offset (%r %d..%d)" % (word.content, word.start_offset, word.end_offset))
    check(Atspi.Text.get_n_selections(email) == 0, "an empty selection is no selection")
    # The login fields take values only (as UI Automation's): a selection is refused, not faked.
    check(not Atspi.Text.set_selection(email, 0, 0, 3), "Text.SetSelection is refused on a login field")
    events.clear()
    check(Atspi.EditableText.delete_text(email, 0, 4), "EditableText.DeleteText is accepted")
    check(wait_for(lambda: text_of(email) == "example.com", 5), "the deletion shows (%r)" % text_of(email))
    deleted = wait_for(lambda: events.matching("object:text-changed:delete"), 5)
    check(bool(deleted) and deleted[0][4] == "mai@" and deleted[0][3] == 4, "a text-deleted event carries the removed text")
    check(Atspi.EditableText.insert_text(email, 0, "my.", 3), "EditableText.InsertText is accepted")
    check(wait_for(lambda: text_of(email) == "my.example.com", 5), "the insertion shows (%r)" % text_of(email))

    # A password discloses nothing, but can be set.
    events.clear()
    check(Atspi.EditableText.set_text_contents(password, "correct horse"), "the password can be set")
    pump(1.0)
    check(Atspi.Text.get_character_count(password) == 0 and Atspi.Text.get_text(password, 0, -1) == "",
          "the password's text is never exposed")
    check(not any(item[4] == "correct horse" for item in events.items), "no event carries the password")

    # An action: invoke the recovery link; the form turns into the recovery form, and a button leads back.
    forgot = find(card, "Forgot your password?", "push button")
    check(forgot is not None and actions(forgot)[:1] == ["click"], "the Forgot your password? button offers a click action first")
    if forgot is not None:
        def snapshot():
            return [(role(n), n.get_name()) for n, _ in walk(window)]

        before = snapshot()
        events.clear()
        check(Atspi.Action.do_action(forgot, 0), "DoAction(click) is accepted")
        check(bool(wait_for(lambda: snapshot() != before, 5)), "the form changes after invoking the button")
        check(bool(wait_for(lambda: events.matching("object:children-changed"), 5)),
              "a children-changed event reports the form's new structure")
        check(bool(events.matching("object:property-change:accessible-name", "Status")),
              "the live status bar's new name arrives as a property change")
        check(any(item[2] == 1 and item[4] == "Status" for item in events.matching("object:announcement")),
              "the live status bar's new name is announced politely")
        print("--- the tree after Forgot your password?", flush=True)
        dump(window)
        print("---", flush=True)
        back = next((n for n, _ in walk(window) if role(n) == "push button" and "Back" in (n.get_name() or "")), None)
        if check(back is not None, "the recovery form has a Back button"):
            Atspi.Action.do_action(back, 0)
            check(bool(wait_for(lambda: find(window, "Log in to MaiCord", "heading") is not None, 5)),
                  "invoking Back restores the sign-in form")

    # A title bar button's action reaches the application's own handler. (Settings opened from the sign-in screen is not
    # projected into the snapshot, as under UI Automation: the tree stays the sign-in form.)
    settings = find(window, "Settings", "push button")
    if check(settings is not None, "the title bar has a Settings button"):
        check(Atspi.Action.do_action(settings, 0), "DoAction on the Settings button is accepted")
        pump(1.0)
        check(find_application() is not None, "the application survives the title bar action")

    # Turning accessibility off again takes the application off the bus.
    set_status(IsEnabled=False, ScreenReaderEnabled=False)
    check(bool(wait_for(lambda: find_application() is None, 10)),
          "the application leaves the registry when accessibility is turned off")


if __name__ == "__main__":
    try:
        main()
    except Exception:  # a crash in the client library is a failure, with its trace
        import traceback
        traceback.print_exc()
        failures += 1
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)
