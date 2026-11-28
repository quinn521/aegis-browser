"""Install the internal Beta node catalogue, anchored to the signed 3X-UI subscription."""

import json
import os
from pathlib import Path
import sys

from private_alpha_nodes import (MAX_NODES, MAX_RESOURCE_BYTES, MAX_URI_BYTES, distinct_json_fields,
                                 read_private, validated_route)

RESOURCE = 'AegisPrivateBetaTestNodes.json'
MAX_INPUT_BYTES = 6 * MAX_RESOURCE_BYTES


def canonicalize(raw, anchor):
    if not 0 < len(raw) <= MAX_INPUT_BYTES:
        raise ValueError('Invalid private Beta catalogue size')
    rows = json.loads(raw, object_pairs_hook=distinct_json_fields)
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_NODES:
        raise ValueError('Expected a nonempty bounded Beta catalogue')
    _, anchor_route = validated_route(anchor.strip(), anchor=True)
    routes = set()
    result = []
    for row in rows:
        if (not isinstance(row, dict) or row.keys() != {'source', 'uri'}
                or row['source'] not in ('3x-ui', 'downloads') or not isinstance(row['uri'], str)):
            raise ValueError('Invalid private Beta catalogue entry')
        _, route = validated_route(row['uri'])
        if not routes and (row['source'] != '3x-ui' or route != anchor_route):
            raise ValueError('The first Beta route must be the signed quota anchor')
        if route in routes:
            raise ValueError('Duplicate private Beta route')
        routes.add(route)
        result.append({'source': row['source'], 'uri': row['uri']})
    output = json.dumps(result, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    if len(output) > MAX_RESOURCE_BYTES:
        raise ValueError('Private Beta catalogue exceeds the app limit')
    return output


def install(channel, source, anchor, subscription, output):
    output.unlink(missing_ok=True)
    if source == '/dev/null':
        return
    if channel != 'beta':
        raise ValueError('The private Beta catalogue requires the Beta channel')
    raw = read_private(source, MAX_INPUT_BYTES)
    node = read_private(anchor, MAX_URI_BYTES).decode('utf-8')
    read_private(subscription, MAX_URI_BYTES)
    with output.open('xb') as bundle_resource:
        bundle_resource.write(canonicalize(raw, node))


if __name__ == '__main__':
    try:
        external = os.environ.get('SCRIPT_INPUT_FILE_6', '/dev/null')
        external_output = os.environ.get('SCRIPT_OUTPUT_FILE_1')
        if external_output:
            Path(external_output).unlink(missing_ok=True)
        if external != '/dev/null':
            import external_beta_nodes
            if (not external_output or os.environ.get('AEGIS_ACCESS_RELEASE_CHANNEL') != 'beta'
                    or any(os.environ[f'SCRIPT_INPUT_FILE_{index}'] != '/dev/null' for index in (2, 3, 4))):
                raise ValueError('Invalid external Beta configuration')
            Path(external_output).write_bytes(external_beta_nodes.canonicalize(read_private(external, MAX_RESOURCE_BYTES)))
        install(os.environ.get('AEGIS_ACCESS_RELEASE_CHANNEL'), os.environ['SCRIPT_INPUT_FILE_2'],
                os.environ['SCRIPT_INPUT_FILE_3'], os.environ['SCRIPT_INPUT_FILE_4'],
                Path(os.environ['SCRIPT_OUTPUT_FILE_0']))
    except (KeyError, OSError, ValueError, TypeError, RecursionError):
        sys.exit('Cannot package the private Beta catalogue. Check the channel and private inputs.')
