#!/usr/bin/env python3
"""Install Orbit's Codex notify hook, preserving unrelated TOML and existing hooks."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import tomllib


def install(binary: str) -> None:
    config = Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))) / "config.toml"
    text = config.read_text(encoding="utf-8") if config.exists() else ""
    data = tomllib.loads(text)
    command = [str(Path(binary).resolve()), "--codex-notify"]
    existing = data.get("notify")
    if existing == command:
        return
    if existing is not None:
        raise ValueError("Codex notify уже настроен. Сохраните существующий обработчик и добавьте вызов Orbit --codex-notify в него вручную.")
    # Root keys must precede all TOML tables. JSON string arrays are valid TOML.
    updated = "notify = " + json.dumps(command, ensure_ascii=False) + "\n" + text
    if tomllib.loads(updated).get("notify") != command:
        raise ValueError("Не удалось проверить конфигурацию Codex")
    config.parent.mkdir(parents=True, exist_ok=True)
    # Never write through a symbolic link or overwrite a concurrent config edit.
    if config.is_symlink():
        raise ValueError("config.toml — символическая ссылка; настройте notify вручную")
    old = config.read_text(encoding="utf-8") if config.exists() else ""
    if old != text:
        raise ValueError("Конфигурация Codex изменилась; повторите подключение")
    if config.exists():
        backup = config.with_name("config.toml.orbit-backup")
        if not backup.exists():
            shutil.copy2(config, backup)
    fd, temporary = tempfile.mkstemp(prefix=".orbit-", dir=config.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(updated)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, config)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


if __name__ == "__main__":
    try:
        install(sys.argv[1])
    except (OSError, ValueError, IndexError) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
