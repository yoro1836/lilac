#!/bin/sh
# Compiles the protocol class and runs its checks on any JDK.
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
javac -Xlint:all -d "$out" "$here/src/com/hexadecinull/vineos/agent/AgentProtocol.java" "$here/test/AgentProtocolTest.java"
java -cp "$out" AgentProtocolTest
