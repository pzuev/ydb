#!/usr/bin/env python3
import sys
import json
from collections import defaultdict

def main():
    input_file = "timing_trace.tsv"
    output_file = "speedscope_evented.json"

    if len(sys.argv) > 1:
        input_file = sys.argv[1]
    if len(sys.argv) > 2:
        output_file = sys.argv[2]

    spans = []
    min_ts = float('inf')

    try:
        with open(input_file, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split('\t')
                if len(parts) < 4:
                    continue

                start_ts = int(parts[0])
                end_ts = int(parts[1])
                thread_name = parts[2]
                span_name = parts[3]

                min_ts = min(min_ts, start_ts)

                spans.append({
                    'start': start_ts,
                    'end': end_ts,
                    'thread': thread_name,
                    'name': span_name,
                    'raw': line,
                })
    except FileNotFoundError:
        print(f"Error: Input file '{input_file}' not found.")
        sys.exit(1)

    if not spans:
        print("No spans found.")
        sys.exit(0)

    # Normalize timestamps
    for span in spans:
        span['start'] -= min_ts
        span['end'] -= min_ts

    # Build shared frames
    frame_names = []
    frame_map = {}

    for span in spans:
        if span['name'] not in frame_map:
            frame_map[span['name']] = len(frame_names)
            frame_names.append({"name": span['name']})

    # Group by thread
    thread_events = defaultdict(list)
    thread_end_values = defaultdict(int)

    span_id = 0

    for span in spans:
        span_id += 1
        thread = span['thread']
        frame_id = frame_map[span['name']]
        start = span['start']
        end = span['end']

        thread_end_values[thread] = max(thread_end_values[thread], end)

        # O event
        thread_events[thread].append({
            'type': 'O',
            'frame': frame_id,
            'at': start,
            # Sort key ensures:
            # 1. Ordered by timestamp
            # 2. If timestamps match, 'C' (close) comes before 'O' (open) unless it's the same span, then it's O before C
            # 3. If multiple 'O's match, the one with the larger end time comes first (parent opens before child)
            'sort_key': (start, span_id, 0, -end),
            'raw': span['raw'],
        })

        # C event
        thread_events[thread].append({
            'type': 'C',
            'frame': frame_id,
            'at': end,
            # Sort key ensures:
            # 1. Ordered by timestamp
            # 2. If timestamps match, 'C' (close) comes before 'O' (open) unless it's the same span, then it's O before C
            # 3. If multiple 'C's match, the one with the smaller start time comes last (parent closes after child)
            'sort_key': (end, span_id, 1, -start),
            'raw': span['raw'],
        })

    profiles = []
    for thread, events in thread_events.items():
        events.sort(key=lambda x: x['sort_key'])

        clean_events = []
        curr_stack = []
        for e in events:
            if e['type'] == 'O':
                curr_stack.append(e)
            else:
                if len(curr_stack) == 0:
                    raise Exception('Close event with no open events on stack: %s' % e['raw'])
                if curr_stack[-1]['frame'] != e['frame']:
                    raise Exception('Close/open frame mismatch. Open frame: [%s] at %d; close frame: [%s] at %d' % (curr_stack[-1]['raw'], curr_stack[-1]['at'], e['raw'], e['at']))
                curr_stack.pop()

            clean_events.append({
                'type': e['type'],
                'frame': e['frame'],
                'at': e['at']
            })

        profiles.append({
            "type": "evented",
            "name": thread,
            "unit": "microseconds",
            "startValue": 0,
            "endValue": thread_end_values[thread],
            "events": clean_events
        })

    speedscope_data = {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "shared": {
            "frames": frame_names
        },
        "profiles": profiles
    }

    with open(output_file, 'w') as f:
        json.dump(speedscope_data, f, indent=2)

    print(f"Successfully converted {len(spans)} spans to {output_file}")

if __name__ == "__main__":
    main()
