#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A private org.freedesktop.secrets service for the isolated test session.

The isolated session has its own D-Bus, so Office's sign-in cannot reach (and
must not reach) the desktop's keyring. Run this inside tests/isolated-run.py to
give it a private one. By default everything lives in this process and is gone
when it exits, and nothing is written to disk. To keep a sign-in across test
sessions set TUXOFFICE_TEST_SECRETS to a file: it is kept with mode 0600, and its
contents are NOT encrypted, so delete it when the tests are done. The service
refuses to start on the desktop's bus.
"""
import base64
import json
import os
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

if not os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"):
    sys.exit("Run this inside the isolated session (tests/isolated-run.py)")

SERVICE = "org.freedesktop.secrets"
ROOT = "/org/freedesktop/secrets"
DEFAULT = ROOT + "/collection/default"
SERVICE_IF = "org.freedesktop.Secret.Service"
COLLECTION_IF = "org.freedesktop.Secret.Collection"
ITEM_IF = "org.freedesktop.Secret.Item"
SESSION_IF = "org.freedesktop.Secret.Session"
PROPS_IF = "org.freedesktop.DBus.Properties"
NO_PROMPT = dbus.ObjectPath("/")


STORE = os.environ.get("TUXOFFICE_TEST_SECRETS")


def save_store(collection):
    if not STORE:
        return
    records = [{"label": item.label, "attributes": item.attributes,
                "secret": base64.b64encode(item.secret).decode(), "content_type": item.content_type}
               for item in collection.items.values()]
    descriptor = os.open(STORE + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as output:
        json.dump(records, output)
    os.replace(STORE + ".tmp", STORE)


class NotSupported(dbus.DBusException):
    _dbus_error_name = "org.freedesktop.DBus.Error.NotSupported"


class NoSuchObject(dbus.DBusException):
    _dbus_error_name = "org.freedesktop.Secret.Error.NoSuchObject"


class Session(dbus.service.Object):
    @dbus.service.method(SESSION_IF)
    def Close(self):
        self.remove_from_connection()


class WithProperties(dbus.service.Object):
    interface = ""

    def properties(self):
        return {}

    @dbus.service.method(PROPS_IF, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.properties()[str(name)]

    @dbus.service.method(PROPS_IF, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.properties(), signature="sv")

    @dbus.service.method(PROPS_IF, in_signature="ssv")
    def Set(self, interface, name, value):
        self.set_property(str(name), value)

    def set_property(self, name, value):
        pass


class Item(WithProperties):
    def __init__(self, bus, path, collection, label, attributes, secret, content_type):
        super().__init__(bus, path)
        self.path = path
        self.collection = collection
        self.label = str(label)
        self.attributes = {str(k): str(v) for k, v in attributes.items()}
        self.secret = bytes(secret)
        self.content_type = str(content_type)
        self.created = int(time.time())

    def properties(self):
        return {
            "Locked": dbus.Boolean(False),
            "Attributes": dbus.Dictionary(self.attributes, signature="ss"),
            "Label": dbus.String(self.label),
            "Created": dbus.UInt64(self.created),
            "Modified": dbus.UInt64(self.created),
        }

    def set_property(self, name, value):
        if name == "Label":
            self.label = str(value)
        elif name == "Attributes":
            self.attributes = {str(k): str(v) for k, v in value.items()}

    def value(self, session):
        return (dbus.ObjectPath(session), dbus.ByteArray(b""), dbus.ByteArray(self.secret),
                dbus.String(self.content_type))

    @dbus.service.method(ITEM_IF, in_signature="o", out_signature="(oayays)")
    def GetSecret(self, session):
        return self.value(session)

    @dbus.service.method(ITEM_IF, in_signature="(oayays)")
    def SetSecret(self, secret):
        self.secret = bytes(secret[2])
        self.content_type = str(secret[3])
        save_store(self.collection)

    @dbus.service.method(ITEM_IF, out_signature="o")
    def Delete(self):
        self.collection.items.pop(self.path, None)
        self.remove_from_connection()
        save_store(self.collection)
        return NO_PROMPT


class Collection(WithProperties):
    def __init__(self, bus, path=DEFAULT, shared=None):
        super().__init__(bus, path)
        self.bus = bus
        self.items = shared.items if shared else {}
        self.counter = shared.counter if shared else [0]

    def properties(self):
        return {
            "Items": dbus.Array([dbus.ObjectPath(p) for p in self.items], signature="o"),
            "Label": dbus.String("TuxOffice test"),
            "Locked": dbus.Boolean(False),
            "Created": dbus.UInt64(0),
            "Modified": dbus.UInt64(0),
        }

    def search(self, attributes):
        wanted = {str(k): str(v) for k, v in attributes.items()}
        return [p for p, item in self.items.items()
                if all(item.attributes.get(k) == v for k, v in wanted.items())]

    @dbus.service.method(COLLECTION_IF, in_signature="a{ss}", out_signature="ao")
    def SearchItems(self, attributes):
        return dbus.Array([dbus.ObjectPath(p) for p in self.search(attributes)], signature="o")

    @dbus.service.method(COLLECTION_IF, in_signature="a{sv}(oayays)b", out_signature="oo")
    def CreateItem(self, properties, secret, replace):
        attributes = properties.get(ITEM_IF + ".Attributes", {})
        label = properties.get(ITEM_IF + ".Label", "")
        if replace:
            for path in self.search(attributes):
                self.items[path].secret = bytes(secret[2])
                self.items[path].label = str(label)
                save_store(self)
                return dbus.ObjectPath(path), NO_PROMPT
        self.counter[0] += 1
        path = f"{DEFAULT}/{self.counter[0]}"
        self.items[path] = Item(self.bus, path, self, label, attributes, secret[2], secret[3])
        save_store(self)
        return dbus.ObjectPath(path), NO_PROMPT

    @dbus.service.method(COLLECTION_IF, out_signature="o")
    def Delete(self):
        raise NotSupported("the test collection cannot be deleted")


class Service(WithProperties):
    def __init__(self, bus):
        super().__init__(bus, ROOT)
        self.bus = bus
        self.collection = Collection(bus)
        # libsecret addresses the default collection by its alias path.
        self.alias = Collection(bus, ROOT + "/aliases/default", self.collection)
        self.sessions = 0

    def properties(self):
        return {"Collections": dbus.Array([dbus.ObjectPath(DEFAULT)], signature="o")}

    @dbus.service.method(SERVICE_IF, in_signature="sv", out_signature="vo")
    def OpenSession(self, algorithm, value):
        if algorithm != "plain":
            raise NotSupported("only the plain algorithm is offered")
        self.sessions += 1
        path = f"{ROOT}/session/{self.sessions}"
        Session(self.bus, path)
        return dbus.String("", variant_level=1), dbus.ObjectPath(path)

    @dbus.service.method(SERVICE_IF, in_signature="a{sv}s", out_signature="oo")
    def CreateCollection(self, properties, alias):
        return dbus.ObjectPath(DEFAULT), NO_PROMPT

    @dbus.service.method(SERVICE_IF, in_signature="a{ss}", out_signature="aoao")
    def SearchItems(self, attributes):
        return (dbus.Array([dbus.ObjectPath(p) for p in self.collection.search(attributes)], signature="o"),
                dbus.Array([], signature="o"))

    @dbus.service.method(SERVICE_IF, in_signature="ao", out_signature="aoo")
    def Unlock(self, objects):
        return objects, NO_PROMPT

    @dbus.service.method(SERVICE_IF, in_signature="ao", out_signature="aoo")
    def Lock(self, objects):
        return dbus.Array([], signature="o"), NO_PROMPT

    @dbus.service.method(SERVICE_IF, in_signature="aoo", out_signature="a{o(oayays)}")
    def GetSecrets(self, items, session):
        found = {}
        for path in items:
            item = self.collection.items.get(str(path))
            if item:
                found[path] = item.value(session)
        return dbus.Dictionary(found, signature="o(oayays)")

    @dbus.service.method(SERVICE_IF, in_signature="s", out_signature="o")
    def ReadAlias(self, name):
        return dbus.ObjectPath(DEFAULT if name == "default" else "/")

    @dbus.service.method(SERVICE_IF, in_signature="so")
    def SetAlias(self, name, collection):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
session_bus = dbus.SessionBus()
if session_bus.name_has_owner(SERVICE):
    sys.exit("A secret service already owns this bus")
name = dbus.service.BusName(SERVICE, session_bus, do_not_queue=True)
service = Service(session_bus)
if STORE and os.path.exists(STORE):
    with open(STORE) as source:
        for record in json.load(source):
            service.collection.counter[0] += 1
            item_path = f"{DEFAULT}/{service.collection.counter[0]}"
            service.collection.items[item_path] = Item(
                session_bus, item_path, service.collection, record["label"], record["attributes"],
                base64.b64decode(record["secret"]), record["content_type"])
print("memory secret service ready", flush=True)
GLib.MainLoop().run()
