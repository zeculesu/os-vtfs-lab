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

@app.route("/api/<method>", methods=["GET"])
def fs_method(method):
    args = request.args
    db = get_db()

    if method == "list":
        parent_ino = int(args["parent_ino"])
        cur = db.execute("SELECT ino, name, type FROM files WHERE parent_ino=?", (parent_ino,))
        out = ""
        for row in cur.fetchall():
            out += f"{row['ino']} {row['name']} {row['type']}\n"
        return out, 200, {'Content-Type': 'text/plain'}
    
    elif method == "lookup":
        ino = int(args["parent_ino"])
        name = args["name"]
        cur = db.execute("SELECT ino, name, type, mode FROM files WHERE parent_ino=? AND name=?", (ino, name))
        row = cur.fetchone()
        if not row:
            return "", 404
        return f"{row['ino']} {row['type']} {row['mode']}"

    elif method == "create":
        name = args["name"]
        parent_ino = int(args["parent_ino"])
        mode = int(args.get("mode", 0o666))
        db.execute("INSERT INTO files (name, parent_ino, type, data, mode) VALUES (?, ?, 'file', ?, ?)",
                   (name, parent_ino, b"", mode))
        db.commit()
        return "ok", 200, {'Content-Type': 'text/plain'}

    elif method == "mkdir":
        name = args["name"]
        parent_ino = int(args["parent_ino"])
        mode = int(args.get("mode", 0o777))
        db.execute("INSERT INTO files (name, parent_ino, type, data, mode) VALUES (?, ?, 'dir', NULL, ?)",
                   (name, parent_ino, mode))
        db.commit()
        return "ok", 200, {'Content-Type': 'text/plain'}


    elif method == "read":
        ino = int(args["ino"])
        cur = db.execute("SELECT data FROM files WHERE ino=? AND type='file'", (ino,))
        row = cur.fetchone()
        if row is None:
            return "error": "not found", 404, {'Content-Type': 'text/plain'}
        return row["data"]
    
    
    elif method == "write":
        ino = int(args["ino"])
        data = args["data"].encode('utf-8')
        offset = int(args.get("offset", 0))
        cur = db.execute("SELECT data FROM files WHERE ino=? AND type='file'", (ino,))
        row = cur.fetchone()
        if row is None:
            return jsonify({"error": "not found"}), 404
        old_data = row["data"] or b""
        new_data = old_data[:offset] + data
        db.execute("UPDATE files SET data=? WHERE ino=?", (new_data, ino))
        db.commit()
        return "ok", 200, {'Content-Type': 'text/plain'}

    elif method == "unlink":
        ino = int(args["ino"])
        db.execute("DELETE FROM files WHERE ino=?", (ino,))
        db.commit()
        return "ok", 200, {'Content-Type': 'text/plain'}

    else:
        return "error", 400, {'Content-Type': 'text/plain'}

if __name__ == "__main__":
    with app.app_context():
        init_db()
    app.run(host="0.0.0.0", port=8080)
