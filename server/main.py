import base64
from flask import Flask, request, jsonify, g
import sqlite3
import os

DB_PATH = "vtfs.db"
app = Flask(__name__)

def get_db():
    db = getattr(g, '_database', None)
    if db is None:
        db = g._database = sqlite3.connect(DB_PATH)
        db.row_factory = sqlite3.Row
    return db

@app.teardown_appcontext
def close_connection(exception):
    db = getattr(g, '_database', None)
    if db is not None:
        db.close()

def init_db():
    db = get_db()
    db.execute('''
    CREATE TABLE IF NOT EXISTS files (
        ino INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        parent_ino INTEGER,
        type TEXT NOT NULL,   -- file or dir
        data BLOB,
        mode INTEGER
    )
    ''')
    db.commit()
    cur = db.execute("SELECT * FROM files WHERE ino=0")
    if cur.fetchone() is None:
        db.execute("INSERT INTO files (ino, name, parent_ino, type, data, mode) VALUES (?, ?, ?, ?, ?, ?)", (0, "/", 0, "dir", None, 0o777))
        db.commit()
@app.route("/api/list")
def list_files():
    parent_ino = int(request.args["parent_ino"])
    db = get_db()
    rows = db.execute("SELECT ino, name, type, mode FROM files WHERE parent_ino=?", (parent_ino,)).fetchall()
    out = "\n".join(f"{r['ino']} {r['type']} {r['mode']} {r['name']}" for r in rows)
    return out, 200, {'Content-Type': 'text/plain'}

@app.route("/api/lookup")
def lookup():
    parent_ino = int(request.args["parent_ino"])
    name = request.args["name"]
    db = get_db()
    row = db.execute("SELECT ino, type, mode FROM files WHERE parent_ino=? AND name=?", (parent_ino, name)).fetchone()
    if row:
        return f"{row['ino']} {row['type']} {row['mode']}", 200, {'Content-Type': 'text/plain'}
    return "ENOENT", 404, {'Content-Type': 'text/plain'}

@app.route("/api/create")
def create_file():
    parent_ino = int(request.args["parent_ino"])
    name = request.args["name"]
    mode = int(request.args["mode"])
    db = get_db()
    cur = db.execute("INSERT INTO files (name, parent_ino, type, data, mode) VALUES (?, ?, ?, ?, ?)",
                     (name, parent_ino, "file", b"", mode))
    db.commit()
    return str(cur.lastrowid), 200, {'Content-Type': 'text/plain'}

@app.route("/api/mkdir")
def mkdir():
    parent_ino = int(request.args["parent_ino"])
    name = request.args["name"]
    mode = int(request.args["mode"])
    db = get_db()
    cur = db.execute("INSERT INTO files (name, parent_ino, type, data, mode) VALUES (?, ?, ?, ?, ?)",
                     (name, parent_ino, "dir", None, mode))
    db.commit()
    return str(cur.lastrowid), 200, {'Content-Type': 'text/plain'}

@app.route("/api/read")
def read_file():
    ino = int(request.args["ino"])
    offset = int(request.args.get("offset", 0))
    length = int(request.args.get("length", 1024))
    db = get_db()
    row = db.execute("SELECT data FROM files WHERE ino=?", (ino,)).fetchone()
    if not row:
        return "ENOENT", 404
    data = row["data"] or b""
    chunk = data[offset:offset+length]
    return base64.b64encode(chunk), 200, {'Content-Type': 'text/plain'}

@app.route("/api/write")
def write_file():
    ino = int(request.args["ino"])
    offset = int(request.args["offset"])
    data = base64.b64decode(request.args["data"])
    db = get_db()
    row = db.execute("SELECT data FROM files WHERE ino=?", (ino,)).fetchone()
    old_data = row["data"] or b""
    if offset > len(old_data):
        old_data += b'\0' * (offset - len(old_data))
    new_data = old_data[:offset] + data
    if offset + len(data) < len(old_data):
        new_data += old_data[offset+len(data):]
    db.execute("UPDATE files SET data=? WHERE ino=?", (new_data, ino))
    db.commit()
    return "0", 200, {'Content-Type': 'text/plain'}

@app.route("/api/unlink")
def unlink():
    ino = int(request.args["ino"])
    db = get_db()
    db.execute("DELETE FROM files WHERE ino=?", (ino,))
    db.commit()
    return "0", 200, {'Content-Type': 'text/plain'}

@app.route("/api/rmdir")
def rmdir():
    ino = int(request.args["ino"])
    db = get_db()
    children = db.execute("SELECT * FROM files WHERE parent_ino=?", (ino,)).fetchall()
    if children:
        return "ENOTEMPTY", 409, {'Content-Type': 'text/plain'}
    db.execute("DELETE FROM files WHERE ino=?", (ino,))
    db.commit()
    return "0", 200, {'Content-Type': 'text/plain'}

if __name__ == "__main__":
    with app.app_context():
        init_db()
    app.run(host="0.0.0.0", port=8080)
