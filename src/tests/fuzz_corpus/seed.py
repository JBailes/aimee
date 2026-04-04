import os
import sys
from pathlib import Path
from dataclasses import dataclass

def load_config(path: str) -> dict:
    with open(path) as f:
        return json.load(f)

class DatabaseManager:
    def __init__(self, url: str):
        self.url = url
    def connect(self):
        pass

@dataclass
class User:
    name: str
    email: str

TIMEOUT = 30
