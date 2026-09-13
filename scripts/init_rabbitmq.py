#!/usr/bin/env python3

import base64
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request


def required(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise RuntimeError(f"required credential is missing: {name}")
    return value


bootstrap_user = required("RABBITMQ_BOOTSTRAP_USER")
bootstrap_password = required("RABBITMQ_BOOTSTRAP_PASSWORD")
max_attempts = int(os.environ.get("RABBITMQ_INIT_MAX_ATTEMPTS", "60"))
authorization = base64.b64encode(
    f"{bootstrap_user}:{bootstrap_password}".encode("utf-8")
).decode("ascii")


def management_request(method: str, path: str, payload: dict | None = None) -> None:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        f"http://rabbitmq:15672{path}",
        data=data,
        method=method,
        headers={
            "Authorization": f"Basic {authorization}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            if response.status < 200 or response.status >= 300:
                raise RuntimeError(f"management request returned status {response.status}")
    except urllib.error.HTTPError as error:
        raise RuntimeError(
            f"management request returned status {error.code} for {path}"
        ) from None


for attempt in range(1, max_attempts + 1):
    try:
        management_request("GET", "/api/overview")
        break
    except (OSError, RuntimeError, urllib.error.URLError):
        if attempt == max_attempts:
            raise RuntimeError("management API did not become ready") from None
        time.sleep(1)


users = {
    "chatnow_transmite": {
        "password": required("CHATNOW_TRANSMITE_MQ_PASSWORD"),
        "configure": r"^chat_msg_exchange$",
        "write": r"^chat_msg_exchange$",
        "read": r"^$",
    },
    "chatnow_message": {
        "password": required("CHATNOW_MESSAGE_MQ_PASSWORD"),
        "configure": r"^(chat_msg_exchange|chat_push_exchange|es_index_exchange|msg_queue_db|msg_queue_es|msg_queue_es_index|msg_push_queue)$",
        "write": r"^(chat_push_exchange|es_index_exchange|msg_queue_db|msg_queue_es|msg_queue_es_index|msg_push_queue)$",
        "read": r"^(chat_msg_exchange|chat_push_exchange|es_index_exchange|msg_queue_db|msg_queue_es|msg_queue_es_index)$",
    },
    "chatnow_push": {
        "password": required("CHATNOW_PUSH_MQ_PASSWORD"),
        "configure": r"^(chat_push_exchange|msg_push_queue)$",
        "write": r"^msg_push_queue$",
        "read": r"^(chat_push_exchange|msg_push_queue)$",
    },
}

for username, settings in users.items():
    encoded_user = urllib.parse.quote(username, safe="")
    management_request(
        "PUT",
        f"/api/users/{encoded_user}",
        {"password": settings["password"], "tags": ""},
    )
    management_request(
        "PUT",
        f"/api/permissions/%2F/{encoded_user}",
        {
            "configure": settings["configure"],
            "write": settings["write"],
            "read": settings["read"],
        },
    )
    management_request("GET", f"/api/permissions/%2F/{encoded_user}")

print("rabbitmq-init: application users and permissions are ready")
