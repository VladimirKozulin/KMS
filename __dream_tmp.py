import sqlite3, json
from datetime import datetime

DB = r'C:\Users\vladi\.local\share\mimocode\mimocode.db'
conn = sqlite3.connect(DB)
conn.row_factory = sqlite3.Row
cur = conn.cursor()

# Get the final assistant message from the KMS analysis session
sid = 'ses_07ef0089effeIPhMWknsTzcNJB'

# Get all messages with full text parts
cur.execute("""
    SELECT m.id, m.agent_id, m.time_created, json_extract(m.data, '$.role') as role
    FROM message m
    WHERE m.session_id=? AND json_extract(m.data, '$.role') = 'assistant'
    ORDER BY m.time_created
""", (sid,))
messages = [dict(r) for r in cur.fetchall()]

for msg in messages:
    cur.execute("""
        SELECT json_extract(p.data, '$.type') as ptype,
               json_extract(p.data, '$.text') as text
        FROM part p
        WHERE p.message_id=? AND json_extract(p.data, '$.type') = 'text'
        ORDER BY p.time_created
    """, (msg['id'],))
    parts = [dict(r) for r in cur.fetchall()]
    for p in parts:
        if p['text']:
            print(f"=== MSG {msg['id']} ===")
            print(p['text'])
            print()

# Also check tasks
print("\n=== TASKS ===")
cur.execute("SELECT * FROM task WHERE session_id=?", (sid,))
tasks = [dict(r) for r in cur.fetchall()]
for t in tasks:
    print(json.dumps(t, indent=2, default=str))

# Check user messages for context
cur.execute("""
    SELECT m.id, json_extract(m.data, '$.role') as role, json_extract(m.data, '$.content') as content
    FROM message m
    WHERE m.session_id=? AND json_extract(m.data, '$.role') = 'user'
    ORDER BY m.time_created
""", (sid,))
for r in cur.fetchall():
    print(f"\nUSER MSG: {r['content'][:500] if r['content'] else 'N/A'}")

conn.close()
