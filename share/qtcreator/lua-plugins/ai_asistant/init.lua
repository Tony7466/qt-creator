-- Copyright (C) 2024 The Qt Company Ltd.
-- SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
local LSP = require('LSP')
local Utils = require('Utils')
local S = require('Settings')
local Gui = require('Gui')
local a = require('async')
local TextEditor = require('TextEditor')
local mm = require('MessageManager')
local Install = require('Install')

Hooks = {}
AutoSuggestionDelay = 2000

local function collectSuggestions(responseTable)
  local suggestions = {}

  if type(responseTable.result) == "table" and type(responseTable.result.completions) == "table" then
    for _, completion in pairs(responseTable.result.completions) do
      if type(completion) == "table" then
        local text = completion.text or ""
        local startLine = completion.range and completion.range.start and completion.range.start.line or 0
        local startCharacter = completion.range and completion.range.start and completion.range.start.character or 0
        local endLine = completion.range and completion.range["end"] and completion.range["end"].line or 0
        local endCharacter = completion.range and completion.range["end"] and completion.range["end"].character or 0

        local suggestion = TextEditor.Suggestion.create(startLine, startCharacter, endLine, endCharacter, text)
        table.insert(suggestions, suggestion)
      end
    end
  end

  return suggestions
end

local function createCommand()
  local cmd = { Settings.binary.expandedValue:nativePath() }
  return cmd
end

local function createInitOptions()
  local llm_config = {
    cppLLM = Settings.cppLLM.stringValue,
    qmlLLM = Settings.qmlLLM.stringValue,
    otherLLM = Settings.otherLLM.stringValue,
    debugLLM = Settings.debugLLM.stringValue,
    reviewLLM = Settings.reviewLLM.stringValue,
    explainLLM = Settings.explainLLM.stringValue
  }

  local auth_token_config = {
    authTokenLama35 = Settings.authTokenLlama3.value,
    authTokenClaude35 = Settings.authTokenClaude35.value
  }

  return {
    llm_config = llm_config,
    auth_token_config = auth_token_config
  }
end


local function installOrUpdateServer()
  local lspPkgInfo = Install.packageInfo("qtaiassistantserver")

  -- TODO: Support multiple platforms

  local url = "https://qtaiassistant.gitlab-pages.qt.io/qtails/downloads/qtaiassistantserver-x86_64-unknown-linux-gnu.gz"
  local res, err = a.wait(Install.install(
    "Do you want to install the qtaiassistantserver?", {
      name = "qtaiassistantserver",
      url = url,
      version = "0.0.1"
    }))

  if not res then
    mm.writeFlashing("Failed to install qtaiassistantserver: " .. err)
    return
  end

  lspPkgInfo = Install.packageInfo("qtaiassistantserver")
  print("Installed:", lspPkgInfo.name, "version:", lspPkgInfo.version, "at", lspPkgInfo.path)

  local binary = "qtaiassistantserver"
  if Utils.HostOsInfo.isWindowsHost() then
    binary = "qtaiassistantserver.exe"
  end

  Settings.binary:setValue(lspPkgInfo.path:resolvePath(binary))
  Settings:apply()
end

IsTryingToInstall = false

local function setupClient()
  Client = LSP.Client.create({
    name = 'AI Assistant Server',
    cmd = createCommand,
    transport = 'stdio',
    initializationOptions = createInitOptions,
    languageFilter = {
      patterns = { '*.*' },
    },
    settings = Settings,
    startBehavior = "AlwaysOn",
    onStartFailed = function()
      a.sync(function()
        if IsTryingToInstall == true then
          return
        end
        IsTryingToInstall = true
        installOrUpdateServer()
        IsTryingToInstall = false
      end)()
    end
  })
end

local function using(tbl)
  local result = _G
  for k, v in pairs(tbl) do result[k] = v end
  return result
end

local function layoutSettings()
  local _ENV = using(Gui)

  local layout = Form {
    Settings.binary, br,
    Row {
      PushButton {
        text = "Try to install AI Assistant Server",
        onClicked = function() a.sync(installOrUpdateServer)() end,
        br,
      },
      st
    },
    br,
    Settings.cppLLM, br,
    Settings.qmlLLM, br,
    Settings.otherLLM, br,
    Settings.debugLLM, br,
    Settings.reviewLLM, br,
    Settings.explainLLM, br,
    Settings.authTokenLlama3, br,
    Settings.authTokenClaude35
  }

  return layout
end

local available_llms = {"Llama 3 70B Fine-Tuned", "Claude 3.5 Sonnet"}

local function createSelectionAspect(settingsKey, displayName)
  return S.SelectionAspect.create({
    settingsKey = settingsKey,
    options = available_llms,
    displayStyle = S.SelectionDisplayStyle.ComboBox,
    displayName = displayName
  })
end

local function addLLMSetting(keySuffix, displayText)
  Settings[keySuffix] = createSelectionAspect("AiAssistant." .. keySuffix, displayText)
end

local function addAuthTokenSetting(llm_name, displayText)
  Settings["authToken" .. llm_name] = S.StringAspect.create({
    settingsKey = "AiAssistant.AuthToken." .. llm_name,
    labelText = displayText .. ":",
    displayStyle = S.StringDisplayStyle.LineEdit,
    defaultValue = "AUTH_TOKEN",
  })
end

local function setupAspect()
  Settings = S.AspectContainer.create({
    autoApply = false,
    layouter = layoutSettings,
  })

  Settings.binary = S.FilePathAspect.create({
    settingsKey = "AiAssistant.Binary",
    displayName = "Binary",
    labelText = "Binary:",
    toolTip = "The path to the AI Assistant Server",
    expectedKind = S.Kind.ExistingCommand,
    defaultPath = Utils.FilePath.fromUserInput("/path/to/server"),
  })

  addLLMSetting("cppLLM", "LLM for C++:")
  addLLMSetting("qmlLLM", "LLM for QML:")
  addLLMSetting("otherLLM", "LLM for other languages:")
  addLLMSetting("debugLLM", "LLM for debug:")
  addLLMSetting("reviewLLM", "LLM for review")
  addLLMSetting("explainLLM", "LLM for explain:")

  addAuthTokenSetting("Llama3", "Llama 3 API authentication Token")
  addAuthTokenSetting("Claude35", "Claude 3.5 API authentication Token")

  Options = S.OptionsPage.create({
    aspectContainer = Settings,
    categoryId = "AiAssistant.OptionsPage",
    displayName = tr("AI Assistant"),
    id = "AiAssistant.Settings",
    displayCategory = "AI Assistant",
    categoryIconPath = PluginSpec.pluginDirectory:resolvePath("images/settingscategory_ai_assistant.png")
  })

  return Settings
end

local function buildRequest()
  local editor = TextEditor.currentEditor()
  if editor == nil then
    print("No editor found")
    return
  end

  local document = editor:document()
  local filePath = document:file()
  local doc_version = Client.documentVersion(filePath)
  if doc_version == -1 then
    print("No document version found")
    return
  end

  local doc_uri = Client.hostPathToServerUri(filePath)
  if doc_uri == nil or doc_uri == "" then
    print("No document uri found")
    return
  end

  local main_cursor = editor:cursor():mainCursor()
  local block = main_cursor:blockNumber()
  local column = main_cursor:columnNumber()

  local request_msg = {
    jsonrpc = "2.0",
    method = "getCompletionsCycling",
    params = {
      doc = {
        position = {
          character = column,
          line = block
        },
        uri = doc_uri,
        version = doc_version
      }
    }
  }

  return request_msg
end

local function completionResponseCallback(response)
  print("completionResponseCallback() called")

  local editor = TextEditor.currentEditor()
  if editor == nil then
    print("No editor found")
    return
  end

  local suggestions = collectSuggestions(response)
  if next(suggestions) == nil then
    print("No suggestions found")
    return
  end

  local document = editor:document()
  document:setSuggestions(suggestions)
end

local function sendRequest(request)
  print("sendRequest() called")

  if Client == nil then
    print("No client found")
    return
  end

  local editor = TextEditor.currentEditor()
  if editor == nil or editor == "" then
    print("No editor found")
    return
  end

  local document = editor:document()
  local result = a.wait(Client:sendMessageWithIdForDocument(document, request))
  completionResponseCallback(result)

end

local function requestSuggestions()
  local main_cursor = TextEditor.currentEditor():cursor():mainCursor()
  if main_cursor == nil then
    print("No cursor found")
    return
  end

  if(main_cursor:hasSelection()) then
    print("Ignoring requestSuggestions() due to cursor selection")
    return
  end

  local request_msg = buildRequest()
  if(request_msg == nil) then
    print("requestSuggestions() failed to build request message")
    return
  end

  sendRequest(request_msg)
end

local function requestSuggestionsSafe()
  local ok, err = pcall(requestSuggestions)
  if not ok then
    print("echo Error fetching: " .. err)
  end
end

AutoSuggestionTimer = Utils.Timer.create(AutoSuggestionDelay, true,
  function() a.sync(requestSuggestionsSafe)() end)

function Hooks.onDocumentContentsChanged(document, position, charsRemoved, charsAdded)
  print("onDocumentContentsChanged() called, position, charsRemoved, charsAdded:", position, charsRemoved, charsAdded)
  AutoSuggestionTimer:start()
end

function Hooks.onCurrentChanged(editor)
  print("onCurrentChanged() called")
  AutoSuggestionTimer:stop()
end

local function setup(parameters)
  setupAspect()
  setupClient()

  Action = require("Action")
  Action.create("Trigger.suggestions", {
    text = "Trigger AI suggestions",
    onTrigger = function() a.sync(requestSuggestionsSafe)() end,
    defaultKeySequences = { "Meta+Shift+A", "Ctrl+Shift+Alt+A" },
  })
end

return {
  setup = function() a.sync(setup)() end,
  Hooks = Hooks,
}
