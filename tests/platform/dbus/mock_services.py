#!/usr/bin/env python3
"""Mock D-Bus services for tests/platform/dbus (python3-dbus and python3-gi; the system libdbus is the oracle).

Run with DBUS_SESSION_BUS_ADDRESS pointing at a private bus (tools/test_dbus.sh). It owns:

  org.maicord.Test                    /org/maicord/Test    generic calls: Describe(any) -> s, Add, Slow, MakeFd, Fire
  org.freedesktop.portal.Desktop      Settings, Notification, OpenURI, Inhibit (version only), ScreenCast
  org.freedesktop.Notifications       the notification daemon
  org.freedesktop.secrets             the Secret Service (plain sessions)
  org.maicord.Mock                    /org/maicord/Mock    the test's control: Take, SetSetting, signals, modes, Quit

Everything a service receives is recorded as one line of text per call (the arguments in a canonical, type-tagged form
decoded by libdbus, not by the code under test); the test fetches the lines with org.maicord.Mock.Take.
"""
import os
import re
import sys

import dbus
import dbus.lowlevel
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

lines = []
modes = {}


def record(text):
    lines.append(text)


def canon(v):
    """A canonical rendering that keeps every D-Bus type."""
    level = getattr(v, 'variant_level', 0)
    inner = canon_value(v)
    return ('v(' * level) + inner + (')' * level)


def canon_value(v):
    if isinstance(v, dbus.Boolean):
        return 'b:%d' % (1 if v else 0)
    if isinstance(v, dbus.Byte):
        return 'y:%d' % int(v)
    if isinstance(v, dbus.Int16):
        return 'n:%d' % int(v)
    if isinstance(v, dbus.UInt16):
        return 'q:%d' % int(v)
    if isinstance(v, dbus.Int32):
        return 'i:%d' % int(v)
    if isinstance(v, dbus.UInt32):
        return 'u:%d' % int(v)
    if isinstance(v, dbus.Int64):
        return 'x:%d' % int(v)
    if isinstance(v, dbus.UInt64):
        return 't:%d' % int(v)
    if isinstance(v, dbus.Double):
        return 'd:%r' % float(v)
    if isinstance(v, dbus.ObjectPath):
        return 'o:%s' % str(v)
    if isinstance(v, dbus.Signature):
        return 'g:%s' % str(v)
    if isinstance(v, dbus.String):
        return 's:%s' % str(v)
    if isinstance(v, dbus.ByteArray):
        return 'ay:%s' % bytes(v).hex()
    if isinstance(v, dbus.types.UnixFd):
        return 'h'
    if isinstance(v, dbus.Dictionary):
        return '{' + ','.join(sorted('%s=%s' % (canon(k), canon(x)) for k, x in v.items())) + '}'
    if isinstance(v, dbus.Array):
        return '[' + ','.join(canon(x) for x in v) + ']'
    if isinstance(v, dbus.Struct):
        return '(' + ','.join(canon(x) for x in v) + ')'
    if isinstance(v, bool):
        return 'b:%d' % (1 if v else 0)
    if isinstance(v, int):
        return 'i:%d' % v
    if isinstance(v, float):
        return 'd:%r' % v
    if isinstance(v, str):
        return 's:%s' % v
    return repr(v)


bus = dbus.SessionBus()
names = [dbus.service.BusName(n, bus) for n in (
    'org.maicord.Test', 'org.maicord.Mock', 'org.freedesktop.portal.Desktop',
    'org.freedesktop.Notifications', 'org.freedesktop.secrets')]


# --- org.maicord.Test ------------------------------------------------------------------------------------------------

def test_filter(connection, message):
    """Describe is answered at the message level so that any signature is accepted and decoded by libdbus."""
    if (message.get_type() == 1 and message.get_path() == '/org/maicord/Test'
            and message.get_interface() == 'org.maicord.Test' and message.get_member() == 'Describe'):
        args = message.get_args_list(byte_arrays=True)
        text = message.get_signature() + ' ' + ' '.join(canon(a) for a in args)
        reply = dbus.lowlevel.MethodReturnMessage(message)
        reply.append(text, signature='s')
        connection.send_message(reply)
        return dbus.lowlevel.HANDLER_RESULT_HANDLED
    return dbus.lowlevel.HANDLER_RESULT_NOT_YET_HANDLED


bus.add_message_filter(test_filter)


class Test(dbus.service.Object):
    def __init__(self):
        super().__init__(bus, '/org/maicord/Test')

    @dbus.service.method('org.maicord.Test', in_signature='ii', out_signature='i')
    def Add(self, a, b):
        return a + b

    @dbus.service.method('org.maicord.Test', in_signature='v', out_signature='v')
    def Echo(self, v):
        return v

    @dbus.service.method('org.maicord.Test', in_signature='u', out_signature='s',
                         async_callbacks=('reply', 'error'))
    def Slow(self, milliseconds, reply, error):
        GLib.timeout_add(milliseconds, lambda: (reply('late'), False)[1])

    @dbus.service.method('org.maicord.Test', in_signature='s', out_signature='h')
    def MakeFd(self, text):
        read_end, write_end = os.pipe()
        os.write(write_end, text.encode())
        os.close(write_end)
        return dbus.types.UnixFd(read_end)

    @dbus.service.method('org.maicord.Test', in_signature='su', out_signature='')
    def Fire(self, text, number):
        self.Ping(text, number)
        self.Other(text)

    @dbus.service.signal('org.maicord.Test', signature='su')
    def Ping(self, text, number):
        pass

    @dbus.service.signal('org.maicord.Test', signature='s')
    def Other(self, text):
        pass

    @dbus.service.method('org.maicord.Test', in_signature='', out_signature='')
    def Fail(self):
        raise dbus.exceptions.DBusException('the mock fails on request', name='org.maicord.Test.Error.Requested')

    @dbus.service.method('org.maicord.Test', in_signature='s', out_signature='')
    def CallBack(self, name):
        """Calls Probe(s) on the caller's unique name: exported methods of the client under test."""
        pass


# --- the portal ------------------------------------------------------------------------------------------------------

settings = {
    'org.freedesktop.appearance': {
        'color-scheme': dbus.UInt32(1),
        'reduced-motion': dbus.UInt32(1),
    },
    'org.gnome.desktop.interface': {
        'enable-animations': dbus.Boolean(False),
        'cursor-blink': dbus.Boolean(True),
        'cursor-blink-time': dbus.Int32(1000),
        'cursor-size': dbus.Int32(32),
        'cursor-theme': dbus.String('Bibata-Modern'),
        'font-name': dbus.String('Cantarell 11'),
        'monospace-font-name': dbus.String('Source Code Pro 10'),
    },
    'org.gnome.desktop.peripherals.mouse': {
        'double-click': dbus.Int32(350),
    },
}


def namespace_selected(namespace, patterns):
    if not patterns:
        return True
    for pattern in patterns:
        pattern = str(pattern)
        if pattern.endswith('*') and namespace.startswith(pattern[:-1]):
            return True
        if pattern == namespace:
            return True
    return False


# --- portal requests and sessions ------------------------------------------------------------------------------------------

portal_state = {'session': None}
WATCHER = 'org.kde.StatusNotifierWatcher'
watcher_state = {'items': []}


def sender_part(sender):
    return str(sender)[1:].replace('.', '_')


class Request(dbus.service.Object):
    def __init__(self, path):
        super().__init__(bus, path)
        self.path = path

    @dbus.service.method('org.freedesktop.portal.Request', in_signature='', out_signature='')
    def Close(self):
        record('Request.Close %s' % self.path)

    @dbus.service.signal('org.freedesktop.portal.Request', signature='ua{sv}')
    def Response(self, code, results):
        pass


class Session(dbus.service.Object):
    def __init__(self, path):
        super().__init__(bus, path)
        self.path = path

    @dbus.service.method('org.freedesktop.portal.Session', in_signature='', out_signature='')
    def Close(self):
        record('Session.Close %s' % self.path)


def make_request(sender, options):
    token = str(options.get('handle_token', 'none'))
    return Request('/org/freedesktop/portal/desktop/request/%s/%s' % (sender_part(sender), token))


def respond(request, code, results, delay=40):
    """The portal answers a Request on its handle after the method returned, as the real one does."""
    def fire():
        request.Response(dbus.UInt32(code), results)
        return False
    GLib.timeout_add(delay, fire)


SCREENCAST = 'org.freedesktop.portal.ScreenCast'
SCREENCAST_PROPERTIES = {'AvailableSourceTypes': dbus.UInt32(3), 'AvailableCursorModes': dbus.UInt32(7)}


class ScreenCastPortal(dbus.service.Object):
    """The ScreenCast portal (screen capture): CreateSession, SelectSources, Start (one stream, node 42, or a cancel with
    mode screencast-cancel), OpenPipeWireRemote (a pipe whose other end is closed: no PipeWire behind it). Its methods
    share names with GlobalShortcuts, so they live in this base class of Portal (dbus-python looks a member up by name
    along the class hierarchy, matching the interface)."""

    @dbus.service.method(SCREENCAST, in_signature='a{sv}', out_signature='o', sender_keyword='sender')
    def CreateSession(self, options, sender=None):
        record('ScreenCast.CreateSession %s' % canon(options))
        request = make_request(sender, options)
        token = str(options.get('session_handle_token', 'maicord'))
        session_path = '/org/freedesktop/portal/desktop/session/%s/%s' % (sender_part(sender), token)
        portal_state['screencast'] = Session(session_path)
        respond(request, 0, dbus.Dictionary({'session_handle': dbus.String(session_path)}, signature='sv'))
        return dbus.ObjectPath(request.path)

    @dbus.service.method(SCREENCAST, in_signature='oa{sv}', out_signature='o', sender_keyword='sender')
    def SelectSources(self, session, options, sender=None):
        record('ScreenCast.SelectSources %s %s' % (canon(session), canon(options)))
        request = make_request(sender, options)
        respond(request, 0, dbus.Dictionary({}, signature='sv'))
        return dbus.ObjectPath(request.path)

    @dbus.service.method(SCREENCAST, in_signature='osa{sv}', out_signature='o', sender_keyword='sender')
    def Start(self, session, parent, options, sender=None):
        record('ScreenCast.Start %s %s %s' % (canon(session), canon(parent), canon(options)))
        request = make_request(sender, options)
        if modes.get('screencast-cancel'):
            respond(request, 1, dbus.Dictionary({}, signature='sv'))
        else:
            stream = dbus.Struct((dbus.UInt32(42), dbus.Dictionary({'source_type': dbus.UInt32(1)}, signature='sv')),
                                 signature='ua{sv}')
            respond(request, 0, dbus.Dictionary({'streams': dbus.Array([stream], signature='(ua{sv})')}, signature='sv'))
        return dbus.ObjectPath(request.path)

    @dbus.service.method(SCREENCAST, in_signature='oa{sv}', out_signature='h')
    def OpenPipeWireRemote(self, session, options):
        record('ScreenCast.OpenPipeWireRemote %s %s' % (canon(session), canon(options)))
        read_end, write_end = os.pipe()
        remote = dbus.types.UnixFd(read_end)  # a duplicate; ours are closed
        os.close(read_end)
        os.close(write_end)
        return remote


class Portal(ScreenCastPortal):
    versions = {
        'org.freedesktop.portal.Settings': 2,
        'org.freedesktop.portal.Notification': 1,
        'org.freedesktop.portal.OpenURI': 3,
        'org.freedesktop.portal.Inhibit': 3,
        'org.freedesktop.portal.GlobalShortcuts': 1,
        'org.freedesktop.portal.FileChooser': 4,
        'org.freedesktop.portal.Screenshot': 2,
        SCREENCAST: 4,
    }

    def __init__(self):
        super().__init__(bus, '/org/freedesktop/portal/desktop')

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        if name == 'version' and str(interface) in self.versions:
            return dbus.UInt32(self.versions[str(interface)])
        if str(interface) == SCREENCAST and str(name) in SCREENCAST_PROPERTIES:
            return SCREENCAST_PROPERTIES[str(name)]
        raise dbus.exceptions.DBusException('no such property', name='org.freedesktop.DBus.Error.UnknownProperty')

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if str(interface) in self.versions:
            return {'version': dbus.UInt32(self.versions[str(interface)])}
        return {}

    @dbus.service.method('org.freedesktop.portal.Settings', in_signature='as', out_signature='a{sa{sv}}')
    def ReadAll(self, namespaces):
        record('ReadAll %s' % canon(namespaces))
        if modes.get('settings-error'):
            raise dbus.exceptions.DBusException('refused', name='org.freedesktop.portal.Error.NotAllowed')
        return {ns: dict(values) for ns, values in settings.items() if namespace_selected(ns, namespaces)}

    @dbus.service.method('org.freedesktop.portal.Settings', in_signature='ss', out_signature='v')
    def Read(self, namespace, key):
        record('Read %s %s' % (namespace, key))
        try:
            return settings[str(namespace)][str(key)]
        except KeyError:
            raise dbus.exceptions.DBusException('no such setting', name='org.freedesktop.portal.Error.NotFound')

    @dbus.service.signal('org.freedesktop.portal.Settings', signature='ssv')
    def SettingChanged(self, namespace, key, value):
        pass

    @dbus.service.method('org.freedesktop.portal.Notification', in_signature='sa{sv}', out_signature='')
    def AddNotification(self, identifier, notification):
        record('AddNotification %s %s' % (canon(identifier), canon(notification)))

    @dbus.service.method('org.freedesktop.portal.Notification', in_signature='s', out_signature='')
    def RemoveNotification(self, identifier):
        record('RemoveNotification %s' % canon(identifier))

    @dbus.service.signal('org.freedesktop.portal.Notification', signature='ssav')
    def ActionInvoked(self, identifier, action, parameter):
        pass

    @dbus.service.method('org.freedesktop.portal.OpenURI', in_signature='ssa{sv}', out_signature='o')
    def OpenURI(self, parent, uri, options):
        record('OpenURI %s %s %s' % (canon(parent), canon(uri), canon(options)))
        if modes.get('openuri-error'):
            raise dbus.exceptions.DBusException('refused', name='org.freedesktop.portal.Error.Failed')
        return dbus.ObjectPath('/org/freedesktop/portal/desktop/request/mock/1')

    @dbus.service.method('org.freedesktop.portal.GlobalShortcuts', in_signature='a{sv}', out_signature='o',
                         sender_keyword='sender')
    def CreateSession(self, options, sender=None):
        record('CreateSession %s' % canon(options))
        if modes.get('shortcuts-error'):
            raise dbus.exceptions.DBusException('no such method', name='org.freedesktop.DBus.Error.UnknownMethod')
        request = make_request(sender, options)
        token = str(options.get('session_handle_token', 'maicord'))
        session_path = '/org/freedesktop/portal/desktop/session/%s/%s' % (sender_part(sender), token)
        portal_state['session'] = Session(session_path)
        respond(request, 0, dbus.Dictionary({'session_handle': dbus.String(session_path)}, signature='sv'))
        return dbus.ObjectPath(request.path)

    @dbus.service.method('org.freedesktop.portal.GlobalShortcuts', in_signature='oa(sa{sv})sa{sv}', out_signature='o',
                         sender_keyword='sender')
    def BindShortcuts(self, session, shortcuts, parent, options, sender=None):
        record('BindShortcuts %s %s %s %s' % (canon(session), canon(shortcuts), canon(parent), canon(options)))
        request = make_request(sender, options)
        if modes.get('bind-cancel'):
            respond(request, 1, dbus.Dictionary({}, signature='sv'))
        else:
            bound = dbus.Array([dbus.Struct((s[0], dbus.Dictionary({'trigger_description': dbus.String('bound')}, signature='sv')),
                                            signature='sa{sv}') for s in shortcuts], signature='(sa{sv})')
            respond(request, 0, dbus.Dictionary({'shortcuts': bound}, signature='sv'))
        return dbus.ObjectPath(request.path)

    @dbus.service.signal('org.freedesktop.portal.GlobalShortcuts', signature='osta{sv}')
    def Activated(self, session, shortcut_id, timestamp, options):
        pass

    @dbus.service.signal('org.freedesktop.portal.GlobalShortcuts', signature='osta{sv}')
    def Deactivated(self, session, shortcut_id, timestamp, options):
        pass

    def file_request(self, name, parent, title, options, sender):
        record('%s %s %s %s' % (name, canon(parent), canon(title), canon(options)))
        request = make_request(sender, options)
        if not modes.get('chooser-hold'):
            code = int(modes.get('file_code', 0))
            uris = [dbus.String(u) for u in modes.get('file_uris', [])]
            results = (dbus.Dictionary({'uris': dbus.Array(uris, signature='s')}, signature='sv') if code == 0
                       else dbus.Dictionary({}, signature='sv'))
            respond(request, code, results)
        return dbus.ObjectPath(request.path)

    @dbus.service.method('org.freedesktop.portal.FileChooser', in_signature='ssa{sv}', out_signature='o',
                         sender_keyword='sender', byte_arrays=True)
    def OpenFile(self, parent, title, options, sender=None):
        return self.file_request('OpenFile', parent, title, options, sender)

    @dbus.service.method('org.freedesktop.portal.FileChooser', in_signature='ssa{sv}', out_signature='o',
                         sender_keyword='sender', byte_arrays=True)
    def SaveFile(self, parent, title, options, sender=None):
        return self.file_request('SaveFile', parent, title, options, sender)

    # The colour picker's eyedropper: answers {color: (ddd)} (mode pick_color: [r, g, b]) or a cancel (pick_code 1).
    @dbus.service.method('org.freedesktop.portal.Screenshot', in_signature='sa{sv}', out_signature='o',
                         sender_keyword='sender')
    def PickColor(self, parent, options, sender=None):
        record('PickColor %s %s' % (canon(parent), canon(options)))
        request = make_request(sender, options)
        code = int(modes.get('pick_code', 0))
        color = modes.get('pick_color', [0.25, 0.5, 1.0])
        results = (dbus.Dictionary({'color': dbus.Struct([dbus.Double(c) for c in color], signature='ddd')},
                                   signature='sv') if code == 0 else dbus.Dictionary({}, signature='sv'))
        respond(request, code, results)
        return dbus.ObjectPath(request.path)


# --- the notification daemon -----------------------------------------------------------------------------------------

class Notifications(dbus.service.Object):
    next_id = 100

    def __init__(self):
        super().__init__(bus, '/org/freedesktop/Notifications')

    @dbus.service.method('org.freedesktop.Notifications', in_signature='susssasa{sv}i', out_signature='u')
    def Notify(self, app_name, replaces_id, icon, summary, body, actions, hints, timeout):
        record('Notify %s' % ' '.join(canon(a) for a in (app_name, replaces_id, icon, summary, body, actions, hints, timeout)))
        size = len(str(body).encode('utf-8'))
        if size > 4000:
            # A body cut short must end between whole entities: the line says how long it was and whether it did.
            record('NotifyLong %d %d' % (size, 0 if re.search(r'&[a-z]{0,3}$', str(body)) else 1))
        if modes.get('notify-error'):
            raise dbus.exceptions.DBusException('no notifications', name='org.freedesktop.Notifications.Error.Failed')
        if int(replaces_id):
            return dbus.UInt32(int(replaces_id))
        Notifications.next_id += 1
        record('NotifyId %d' % Notifications.next_id)
        return dbus.UInt32(Notifications.next_id)

    @dbus.service.method('org.freedesktop.Notifications', in_signature='u', out_signature='')
    def CloseNotification(self, identifier):
        record('CloseNotification %s' % canon(identifier))
        self.NotificationClosed(identifier, dbus.UInt32(3))

    @dbus.service.method('org.freedesktop.Notifications', in_signature='', out_signature='as')
    def GetCapabilities(self):
        if modes.get('no-markup'):
            return ['actions', 'body', 'persistence']
        return ['actions', 'body', 'body-markup', 'persistence']

    @dbus.service.method('org.freedesktop.Notifications', in_signature='', out_signature='ssss')
    def GetServerInformation(self):
        return 'MockDaemon', 'maicord', '1.0', '1.2'

    @dbus.service.signal('org.freedesktop.Notifications', signature='us')
    def ActionInvoked(self, identifier, key):
        pass

    @dbus.service.signal('org.freedesktop.Notifications', signature='uu')
    def NotificationClosed(self, identifier, reason):
        pass

    @dbus.service.signal('org.freedesktop.Notifications', signature='us')
    def ActivationToken(self, identifier, token):
        pass


# --- the Secret Service ----------------------------------------------------------------------------------------------

SECRETS = '/org/freedesktop/secrets'
items = {}  # path -> dict(label, attributes, secret, content_type)
state = {'locked': False, 'sessions': 0, 'next_item': 1}


class Item(dbus.service.Object):
    def __init__(self, path):
        super().__init__(bus, path)
        self.path = path

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        item = items[self.path]
        if name == 'Locked':
            return dbus.Boolean(state['locked'])
        if name == 'Label':
            return dbus.String(item['label'])
        if name == 'Attributes':
            return dbus.Dictionary(item['attributes'], signature='ss')
        raise dbus.exceptions.DBusException('no such property', name='org.freedesktop.DBus.Error.UnknownProperty')

    @dbus.service.method('org.freedesktop.Secret.Item', in_signature='o', out_signature='(oayays)', byte_arrays=True)
    def GetSecret(self, session):
        record('GetSecret %s' % self.path)
        if state['locked']:
            raise dbus.exceptions.DBusException('locked', name='org.freedesktop.Secret.Error.IsLocked')
        item = items[self.path]
        return (session, dbus.ByteArray(b''), dbus.ByteArray(item['secret']), item['content_type'])

    @dbus.service.method('org.freedesktop.Secret.Item', in_signature='', out_signature='o')
    def Delete(self):
        record('Delete %s' % self.path)
        items.pop(self.path, None)
        return dbus.ObjectPath('/')


class Collection(dbus.service.Object):
    def __init__(self, path):
        super().__init__(bus, path)

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        if name == 'Locked':
            return dbus.Boolean(state['locked'])
        raise dbus.exceptions.DBusException('no such property', name='org.freedesktop.DBus.Error.UnknownProperty')

    @dbus.service.method('org.freedesktop.Secret.Collection', in_signature='a{sv}(oayays)b', out_signature='oo',
                         byte_arrays=True)
    def CreateItem(self, properties, secret, replace):
        label = str(properties.get('org.freedesktop.Secret.Item.Label', ''))
        attributes = {str(k): str(v) for k, v in properties.get('org.freedesktop.Secret.Item.Attributes', {}).items()}
        record('CreateItem label=%s attrs=%s secret=%s type=%s replace=%d session=%s parameters=%s' % (
            label, canon(dbus.Dictionary(attributes, signature='ss')), bytes(secret[2]).hex(), str(secret[3]),
            1 if replace else 0, str(secret[0]), bytes(secret[1]).hex()))
        if state['locked']:
            return dbus.ObjectPath('/'), dbus.ObjectPath('/org/freedesktop/secrets/prompt/1')
        if modes.get('secret-create-error'):
            raise dbus.exceptions.DBusException('cannot create', name='org.freedesktop.Secret.Error.Failed')
        if replace:
            for path in [p for p, item in items.items() if item['attributes'] == attributes]:
                items.pop(path)
        path = '%s/collection/login/%d' % (SECRETS, state['next_item'])
        state['next_item'] += 1
        items[path] = {'label': label, 'attributes': attributes, 'secret': bytes(secret[2]), 'content_type': str(secret[3])}
        Item(path)
        return dbus.ObjectPath(path), dbus.ObjectPath('/')


class SecretService(dbus.service.Object):
    def __init__(self):
        super().__init__(bus, SECRETS)

    @dbus.service.method('org.freedesktop.Secret.Service', in_signature='sv', out_signature='vo')
    def OpenSession(self, algorithm, argument):
        record('OpenSession %s %s' % (canon(algorithm), canon(argument)))
        if str(algorithm) != 'plain':
            raise dbus.exceptions.DBusException('unsupported', name='org.freedesktop.DBus.Error.NotSupported')
        state['sessions'] += 1
        return dbus.String('', variant_level=1), dbus.ObjectPath('%s/session/%d' % (SECRETS, state['sessions']))

    @dbus.service.method('org.freedesktop.Secret.Service', in_signature='a{ss}', out_signature='aoao')
    def SearchItems(self, attributes):
        record('SearchItems %s' % canon(attributes))
        wanted = {str(k): str(v) for k, v in attributes.items()}
        found = [dbus.ObjectPath(p) for p, item in items.items()
                 if all(item['attributes'].get(k) == v for k, v in wanted.items())]
        return (dbus.Array([], signature='o'), found) if state['locked'] else (found, dbus.Array([], signature='o'))

    @dbus.service.method('org.freedesktop.Secret.Service', in_signature='ao', out_signature='aoo')
    def Unlock(self, objects):
        record('Unlock %s' % canon(objects))
        return dbus.Array([], signature='o'), dbus.ObjectPath('/org/freedesktop/secrets/prompt/1')

    @dbus.service.method('org.freedesktop.Secret.Service', in_signature='s', out_signature='o')
    def ReadAlias(self, name):
        record('ReadAlias %s' % name)
        return dbus.ObjectPath('%s/collection/login' % SECRETS) if str(name) == 'default' else dbus.ObjectPath('/')

    @dbus.service.method('org.freedesktop.Secret.Service', in_signature='aoo', out_signature='a{o(oayays)}',
                         byte_arrays=True)
    def GetSecrets(self, paths, session):
        record('GetSecrets %s %s' % (canon(paths), str(session)))
        result = {}
        for p in paths:
            item = items.get(str(p))
            if item:
                result[p] = (session, dbus.ByteArray(b''), dbus.ByteArray(item['secret']), item['content_type'])
        return result

    @dbus.service.method('org.freedesktop.Secret.Session', in_signature='', out_signature='')
    def Close(self):
        pass


# --- the control -----------------------------------------------------------------------------------------------------

class Control(dbus.service.Object):
    def __init__(self, portal, notifications):
        super().__init__(bus, '/org/maicord/Mock')
        self.portal = portal
        self.notifications = notifications

    @dbus.service.method('org.maicord.Mock', in_signature='', out_signature='as')
    def Take(self):
        taken = list(lines)
        del lines[:]
        return taken

    @dbus.service.method('org.maicord.Mock', in_signature='ssv', out_signature='')
    def SetSetting(self, namespace, key, value):
        settings.setdefault(str(namespace), {})[str(key)] = value
        self.portal.SettingChanged(namespace, key, value)

    @dbus.service.method('org.maicord.Mock', in_signature='su', out_signature='')
    def NotifyAction(self, key, identifier):
        self.notifications.ActionInvoked(dbus.UInt32(identifier), key)

    @dbus.service.method('org.maicord.Mock', in_signature='uu', out_signature='')
    def NotifyClosed(self, identifier, reason):
        self.notifications.NotificationClosed(identifier, reason)

    @dbus.service.method('org.maicord.Mock', in_signature='us', out_signature='')
    def NotifyToken(self, identifier, token):
        self.notifications.ActivationToken(identifier, token)

    @dbus.service.method('org.maicord.Mock', in_signature='ss', out_signature='')
    def PortalAction(self, identifier, action):
        self.portal.ActionInvoked(identifier, action, dbus.Array([], signature='v'))

    @dbus.service.method('org.maicord.Mock', in_signature='sb', out_signature='')
    def SetMode(self, name, value):
        if str(name) == 'secret-locked':
            state['locked'] = bool(value)
        else:
            modes[str(name)] = bool(value)

    @dbus.service.method('org.maicord.Mock', in_signature='', out_signature='')
    def Reset(self):
        items.clear()
        modes.clear()
        state['locked'] = False
        del lines[:]

    @dbus.service.method('org.maicord.Mock', in_signature='ss', out_signature='')
    def StoreSecret(self, attribute_key, attribute_value):
        """Plants a secret as if another program had stored it (a migration test)."""
        path = '%s/collection/login/%d' % (SECRETS, state['next_item'])
        state['next_item'] += 1
        items[path] = {'label': 'planted', 'attributes': {str(attribute_key): str(attribute_value)},
                       'secret': b'\x01' * 32, 'content_type': 'application/octet-stream'}
        Item(path)

    @dbus.service.method('org.maicord.Mock', in_signature='uas', out_signature='')
    def SetFileResult(self, code, uris):
        modes['file_code'] = int(code)
        modes['file_uris'] = [str(u) for u in uris]

    @dbus.service.method('org.maicord.Mock', in_signature='sb', out_signature='')
    def ShortcutState(self, shortcut_id, active):
        session = portal_state['session']
        if session is None:
            return
        arguments = (dbus.ObjectPath(session.path), shortcut_id, dbus.UInt64(12345), dbus.Dictionary({}, signature='sv'))
        if active:
            self.portal.Activated(*arguments)
        else:
            self.portal.Deactivated(*arguments)

    @dbus.service.method('org.maicord.Mock', in_signature='b', out_signature='')
    def WatcherSet(self, present):
        if present:
            bus.request_name(WATCHER, dbus.bus.NAME_FLAG_DO_NOT_QUEUE)
        else:
            bus.release_name(WATCHER)
            del watcher_state['items'][:]

    @dbus.service.method('org.maicord.Mock', in_signature='ssssssav', out_signature='')
    def Invoke(self, tag, dest, path, interface, member, signature, arguments):
        """A call from the mock to the code under test; the answer is logged as `Result <tag> ok|error ...`.
        A destination of @item is the last service registered with the watcher."""
        target = watcher_state['items'][-1] if str(dest) == '@item' and watcher_state['items'] else str(dest)

        def done(*values):
            record('Result %s ok %s' % (tag, ' '.join(canon(v) for v in values)))

        def failed(error):
            record('Result %s error %s' % (tag, error.get_dbus_name()))

        bus.call_async(target, str(path), str(interface), str(member), str(signature), tuple(arguments), done, failed,
                       timeout=5, byte_arrays=True)

    @dbus.service.method('org.maicord.Mock', in_signature='', out_signature='')
    def Quit(self):
        GLib.idle_add(loop.quit)


loop = GLib.MainLoop()
Test()
portal = Portal()
notifications = Notifications()
SecretService()
Collection('%s/collection/login' % SECRETS)
Control(portal, notifications)


class Watcher(dbus.service.Object):
    """org.kde.StatusNotifierWatcher: its name is owned only while a test asks for it (WatcherSet)."""

    def __init__(self):
        super().__init__(bus, '/StatusNotifierWatcher')

    @dbus.service.method(WATCHER, in_signature='s', out_signature='')
    def RegisterStatusNotifierItem(self, service):
        record('RegisterItem %s' % service)
        watcher_state['items'].append(str(service))
        self.StatusNotifierItemRegistered(service)

    @dbus.service.method(WATCHER, in_signature='s', out_signature='')
    def RegisterStatusNotifierHost(self, service):
        record('RegisterHost %s' % service)

    @dbus.service.signal(WATCHER, signature='s')
    def StatusNotifierItemRegistered(self, service):
        pass

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        values = {'IsStatusNotifierHostRegistered': dbus.Boolean(True), 'ProtocolVersion': dbus.Int32(0),
                  'RegisteredStatusNotifierItems': dbus.Array(watcher_state['items'], signature='s')}
        return values[str(name)]


def on_launcher_entry(uri, properties, path=None):
    record('LauncherEntry %s %s %s' % (canon(uri), canon(properties), path))


Watcher()
bus.add_signal_receiver(on_launcher_entry, signal_name='Update', dbus_interface='com.canonical.Unity.LauncherEntry',
                        path_keyword='path')


def on_item_signal(*arguments, member=None, path=None):
    record('ItemSignal %s %s %s' % (member, path, ' '.join(canon(a) for a in arguments)))


for interface in ('org.kde.StatusNotifierItem', 'com.canonical.dbusmenu'):
    bus.add_signal_receiver(on_item_signal, dbus_interface=interface, member_keyword='member', path_keyword='path')
print('ready', flush=True)
loop.run()
