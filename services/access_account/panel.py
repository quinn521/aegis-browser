"""Read a dedicated finite-period client's counters from 3X-UI v3.11.0."""

from contextlib import closing
import http.client
import json
import ssl
from urllib.parse import quote, urlsplit

from .store import Unavailable


class Panel:
    def __init__(self, base_url, api_token):
        url = urlsplit(base_url)
        if (not url.hostname or url.username is not None or url.password is not None
                or url.query or url.fragment or not api_token or not api_token.isascii()
                or not api_token.isprintable() or any(ch.isspace() for ch in api_token)):
            raise ValueError("invalid panel configuration")
        if url.scheme != "https" and not (url.scheme == "http" and url.hostname == "127.0.0.1"):
            raise ValueError("panel requires HTTPS or local 127.0.0.1 HTTP")
        self.host = url.hostname
        self.port = url.port
        self.tls = url.scheme == "https"
        self.prefix = url.path.rstrip("/") + "/panel/api/clients/traffic/"
        self.token = api_token

    def read(self, client, now_ms):
        if client in ("", ".", "..") or "/" in client or "\\" in client:
            raise Unavailable("invalid panel client binding")
        # http.client does not follow redirects or consult environment proxy settings.
        target = self.prefix + quote(client, safe="")
        headers = {"Authorization": "Bearer " + self.token, "Accept": "application/json"}
        try:
            if self.tls:
                conn = http.client.HTTPSConnection(self.host, self.port, timeout=5,
                                                   context=ssl.create_default_context())
            else:
                conn = http.client.HTTPConnection(self.host, self.port, timeout=5)
            with closing(conn):
                conn.request("GET", target, headers=headers)
                response = conn.getresponse()
                if response.status != 200:
                    raise Unavailable("panel request failed")
                body = response.read(65537)
                if len(body) > 65536:
                    raise Unavailable("panel response too large")
                result = json.loads(body)
        except (OSError, http.client.HTTPException, ValueError, RecursionError) as error:
            raise Unavailable("invalid panel response") from error

        received = now_ms()
        if not isinstance(result, dict) or result.get("success") is not True:
            raise Unavailable("panel read failed")
        row = result.get("obj")
        if not isinstance(row, dict) or row.get("email") != client:
            raise Unavailable("panel client unavailable")
        for key in ("total", "up", "down", "expiryTime"):
            value = row.get(key)
            if type(value) is not int or not 0 <= value <= 2**63 - 1:
                raise Unavailable("invalid panel counter or limit")
        quota, up, down, expiry = (row[k] for k in ("total", "up", "down", "expiryTime"))
        if (not quota or not 0 < expiry <= 2**53 - 1
                or any(type(row.get(k)) is not int or row[k] != 0
                       for k in ("reset", "resetDay", "resetWeekday"))):
            raise Unavailable("a dedicated finite plan with an absolute expiry is required")
        if type(row.get("enable")) is not bool or (not row["enable"] and up + down < quota
                                                    and received < expiry):
            raise Unavailable("panel client disabled")
        return dict(quota_bytes=quota, uploaded_bytes=up, downloaded_bytes=down,
                    expires_at=expiry, received_at=received, measured_at=None)
