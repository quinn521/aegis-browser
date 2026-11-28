"""Prepare the connection-only node list for an externally distributed Beta."""
import base64
import json

from private_alpha_nodes import MAX_NODES, MAX_RESOURCE_BYTES, distinct_json_fields, validated_route

RESOURCE = 'AegisExternalBetaNodes.json'
PRIVATE_RESOURCES = {'AegisPrivateTestNode.vless', 'AegisPrivateTestSubscription.json',
                     'AegisPrivateBetaTestNodes.json', 'AegisAlphaTestNodes.json'}


def connection_uri(uri):
    _, route = validated_route(uri)
    clean = uri.partition('#')[0]
    if clean.startswith('vmess://'):
        payload = clean.removeprefix('vmess://')
        data = json.loads(base64.b64decode(payload + '=' * (-len(payload) % 4), altchars=b'-_', validate=True),
                          object_pairs_hook=distinct_json_fields)
        data.pop('ps', None)
        encoded = json.dumps(data, separators=(',', ':'), sort_keys=True).encode()
        clean = 'vmess://' + base64.b64encode(encoded).decode()
    if validated_route(clean)[1] != route:
        raise ValueError('Removing display metadata changed the connection')
    return clean, route


def canonicalize(raw):
    if not 0 < len(raw) <= MAX_RESOURCE_BYTES:
        raise ValueError('Invalid external Beta catalogue size')
    rows = json.loads(raw, object_pairs_hook=distinct_json_fields)
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_NODES:
        raise ValueError('Expected a bounded list of external Beta nodes')
    nodes, routes = [], set()
    for row in rows:
        if not isinstance(row, dict) or set(row) not in ({'uri'}, {'uri', 'quotaBytes'}):
            raise ValueError('Unexpected external node field')
        clean, route = connection_uri(row['uri'])
        if route in routes:
            raise ValueError('Duplicate external Beta node')
        routes.add(route)
        node = {'uri': clean}
        if 'quotaBytes' in row:
            quota = row['quotaBytes']
            if type(quota) is not int or not 0 < quota <= 2**63 - 1:
                raise ValueError('Invalid node quota')
            node['quotaBytes'] = quota
        nodes.append(node)
    return json.dumps(nodes, separators=(',', ':'), ensure_ascii=False).encode()


def verify_archive(archive, root, external):
    names = archive.namelist()
    def basename(member):
        return member.replace('\\', '/').rstrip('/').rsplit('/', 1)[-1].casefold()

    found = [n for n in names if basename(n) == RESOURCE.casefold()]
    if found != ([root + RESOURCE] if external else []):
        raise ValueError('Unexpected external Beta node catalogue placement')
    if external:
        forbidden = {name.casefold() for name in PRIVATE_RESOURCES}
        if any(basename(n) in forbidden for n in names):
            raise ValueError('External Beta contains a private subscription or internal node resource')
        if archive.getinfo(found[0]).file_size > MAX_RESOURCE_BYTES:
            raise ValueError('External Beta node catalogue too large')
        raw = archive.read(found[0])
        if canonicalize(raw) != raw:
            raise ValueError('External Beta node catalogue contains non-connection metadata')
        return raw
