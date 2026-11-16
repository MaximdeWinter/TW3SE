// VS Code extension: the WitcherScript debugger for TW3SE. The adapter runs inside VS Code (adapter.js).
'use strict';
const vscode = require('vscode');
const { Session } = require('./adapter');

function activate(context) {
  context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('witcherscript', {
    createDebugAdapterDescriptor(session) {
      const folders = (vscode.workspace.workspaceFolders || []).map((f) => f.uri.fsPath);
      return new vscode.DebugAdapterInlineImplementation(new Session(session.configuration, folders));
    },
  }));
}

function deactivate() {}

module.exports = { activate, deactivate };
