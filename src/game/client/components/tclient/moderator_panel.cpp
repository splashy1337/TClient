#include "moderator_panel.h"

#include <base/log.h>
#include <base/system.h>
#include <engine/external/json-parser/json.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/ui.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include <base/os.h>

static constexpr int JSON_POLL_INTERVAL_SECONDS = 5;

void CModeratorPanel::ConTogglePanel(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CModeratorPanel *>(pUserData)->Toggle();
}

void CModeratorPanel::ConAddTodo(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CModeratorPanel *>(pUserData)->AddTodo(pResult->GetString(0));
}

void CModeratorPanel::ConClearTodos(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CModeratorPanel *>(pUserData)->ClearTodos();
}

void CModeratorPanel::OnConsoleInit()
{
	Console()->Register("mc_toggle_moderator_panel", "", CFGFLAG_CLIENT, ConTogglePanel, this,
		"Toggle the Moderator Client moderator overlay panel");
	Console()->Register("mc_add_todo", "s[name]", CFGFLAG_CLIENT, ConAddTodo, this,
		"Mark a player name as a moderation to-do (highlighted red)");
	Console()->Register("mc_clear_todos", "", CFGFLAG_CLIENT, ConClearTodos, this,
		"Clear the to-do player list");
}

void CModeratorPanel::OnInit()
{
	LoadStoredEntries();
	if(g_Config.m_McAutoStartDiscordBridge)
		StartDiscordBridge();
}

void CModeratorPanel::OnShutdown()
{
	if(m_StoredEntriesDirty)
	{
		SaveStoredEntries();
		m_StoredEntriesDirty = false;
	}
	if(m_BridgeProcess != INVALID_PROCESS && process_is_alive(m_BridgeProcess))
	{
		process_kill(m_BridgeProcess);
	}
	m_BridgeProcess = INVALID_PROCESS;
}

void CModeratorPanel::OnReset()
{
	m_KogCheckDone = false;
	m_vPlayers.clear();
}

bool CModeratorPanel::IsBridgeRunning() const
{
	return m_BridgeProcess != INVALID_PROCESS && process_is_alive(m_BridgeProcess);
}

void CModeratorPanel::StartDiscordBridge()
{
	char aScriptPath[IO_MAX_PATH_LENGTH];
	str_copy(aScriptPath, g_Config.m_McDiscordBridgeScript);

	// Self-heal: if the configured path is empty or no longer exists (e.g. game was moved),
	// search for moderatorclient_scripts/discord_bridge.py relative to the executable.
	if(aScriptPath[0] == '\0' || !fs_is_file(aScriptPath))
	{
		const char *apCandidates[] = {
			"../../moderatorclient_scripts/discord_bridge.py", // tclient/build/release/DDNet.exe -> tclient/moderatorclient_scripts
			"../moderatorclient_scripts/discord_bridge.py",
			"moderatorclient_scripts/discord_bridge.py",
		};
		bool Found = false;
		for(const char *pRel : apCandidates)
		{
			char aAbs[IO_MAX_PATH_LENGTH];
			Storage()->GetBinaryPathAbsolute(pRel, aAbs, sizeof(aAbs));
			if(fs_is_file(aAbs))
			{
				str_copy(aScriptPath, aAbs);
				str_copy(g_Config.m_McDiscordBridgeScript, aAbs, sizeof(g_Config.m_McDiscordBridgeScript));
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod",
					"discord bridge: auto-detected script path");
				Found = true;
				break;
			}
		}
		if(!Found)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod",
				aScriptPath[0] == '\0' ?
					"discord bridge: no script path set and could not auto-detect (Settings -> Moderation)" :
					"discord bridge: configured script not found and auto-detect failed");
			return;
		}
	}

	if(IsBridgeRunning())
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod",
			"discord bridge: already running");
		return;
	}
	const char *pArgs[3] = {aScriptPath, "--channel-id", g_Config.m_McDiscordChannelId};
	int NumArgs = g_Config.m_McDiscordChannelId[0] != '\0' ? 3 : 1;
	m_BridgeProcess = process_execute("pythonw.exe", EShellExecuteWindowState::BACKGROUND, pArgs, NumArgs);
	if(m_BridgeProcess == INVALID_PROCESS)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod",
			"discord bridge: failed to spawn pythonw.exe (not installed or not on PATH?)");
	}
	else
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", "discord bridge: started");
	}
}

bool CModeratorPanel::IsActive() const { return g_Config.m_McModeratorPanelOpen != 0; }
void CModeratorPanel::SetActive(bool Active) { g_Config.m_McModeratorPanelOpen = Active ? 1 : 0; }
void CModeratorPanel::Toggle() { SetActive(!IsActive()); }

bool CModeratorPanel::OnInput(const IInput::CEvent &Event)
{
	if(!IsActive())
		return false;
	if(Event.m_Flags & IInput::FLAG_PRESS)
	{
		if(Event.m_Key == KEY_ESCAPE)
		{
			SetActive(false);
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_WHEEL_UP)
		{
			float &Scroll = (m_ActiveTab == 0) ? m_ScrollY : m_LogScrollY;
			Scroll = std::max(0.0f, Scroll - 16.0f);
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_WHEEL_DOWN)
		{
			float &Scroll = (m_ActiveTab == 0) ? m_ScrollY : m_LogScrollY;
			Scroll += 16.0f;
			return true;
		}
		if(Event.m_Key == KEY_MOUSE_1 || Event.m_Key == KEY_MOUSE_2 || Event.m_Key == KEY_MOUSE_3)
		{
			const vec2 Mouse = Ui()->MousePos();
			if(Event.m_Key == KEY_MOUSE_1)
			{
				for(const auto &C : m_vClickables)
				{
					if(Mouse.x >= C.m_X && Mouse.x <= C.m_X + C.m_W &&
						Mouse.y >= C.m_Y && Mouse.y <= C.m_Y + C.m_H)
					{
						if(C.m_Action == 0)
							CopyBanLine(C.m_Data);
						else if(C.m_Action == 1 && C.m_Data >= 0 && C.m_Data < (int)m_vPlayers.size())
							DownloadPlayerAttachments(m_vPlayers[C.m_Data].m_aName);
						else if(C.m_Action == 2)
						{
							m_ActiveTab = C.m_Data;
							m_LogScrollY = 0.0f;
							m_ScrollY = 0.0f;
						}
						else if(C.m_Action == 3)
							CopyDetectionStatusLine(C.m_Data);
						else if(C.m_Action == 4)
							GameClient()->m_ChatWatcher.ClearLog();
						else if(C.m_Action == 5)
							GameClient()->m_ChatWatcher.RemoveLogEntry((size_t)C.m_Data);
						else if(C.m_Action == 6 && C.m_Data >= 0 && C.m_Data < (int)m_vPlayers.size())
						{
							const CTodoEntry *pTodo = FindTodo(m_vPlayers[C.m_Data].m_aName);
							if(pTodo && !pTodo->m_PostUrl.empty())
								os_open_link(pTodo->m_PostUrl.c_str());
						}
						else if(C.m_Action == 7)
							OpenEvidenceFolder();
						else if(C.m_Action == 8 && C.m_Data >= 0 && C.m_Data < (int)m_vPlayers.size())
							MarkDone(C.m_Data);
						else if(C.m_Action == 9 && C.m_Data >= 0 && C.m_Data < (int)m_vStoredEntries.size())
						{
							if(!m_vStoredEntries[C.m_Data].m_FullStatusLine.empty())
								Input()->SetClipboardText(m_vStoredEntries[C.m_Data].m_FullStatusLine.c_str());
						}
						return true;
					}
				}
			}
			// Consume all mouse button events when panel is active — don't let anything
			// behind the full-screen overlay get clicked.
			return true;
		}
	}
	return false;
}

void CModeratorPanel::OnKogServerConnect()
{
	if(g_Config.m_McKogLoginCode[0] != '\0')
	{
		char aLogin[160];
		str_format(aLogin, sizeof(aLogin), "/login %s", g_Config.m_McKogLoginCode);
		GameClient()->m_Chat.SendChatQueued(aLogin);
	}
	if(g_Config.m_McRconPassword[0] != '\0')
	{
		const char *pUser = g_Config.m_McRconUsername;
		const char *pPass = g_Config.m_McRconPassword;
		char aUserEsc[160], aPassEsc[160];
		{
			char *p = aUserEsc;
			str_escape(&p, pUser, aUserEsc + sizeof(aUserEsc));
		}
		{
			char *p = aPassEsc;
			str_escape(&p, pPass, aPassEsc + sizeof(aPassEsc));
		}
		char aCmd[320];
		if(pUser[0] != '\0')
			str_format(aCmd, sizeof(aCmd), "rcon_login \"%s\" \"%s\"", aUserEsc, aPassEsc);
		else
			str_format(aCmd, sizeof(aCmd), "rcon_auth \"%s\"", aPassEsc);
		Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
	}
}

bool CModeratorPanel::IsTodo(const char *pName) const
{
	if(!pName || pName[0] == '\0')
		return false;
	for(const auto &Todo : m_vTodos)
	{
		if(str_comp(Todo.c_str(), pName) == 0)
			return true;
	}
	return false;
}

void CModeratorPanel::AddTodo(const char *pName)
{
	if(!pName || pName[0] == '\0' || IsTodo(pName))
		return;
	m_vTodos.emplace_back(pName);
}

void CModeratorPanel::ClearTodos()
{
	m_vTodos.clear();
	m_vTodoEntries.clear();
}

const CModeratorPanel::CTodoEntry *CModeratorPanel::FindTodo(const char *pName) const
{
	if(!pName || pName[0] == '\0')
		return nullptr;
	for(const auto &Entry : m_vTodoEntries)
	{
		if(str_comp(Entry.m_Name.c_str(), pName) == 0)
			return &Entry;
	}
	return nullptr;
}

bool CModeratorPanel::ParseStatusLine(const char *pLine, CPlayerEntry *pOut)
{
	const char *pIdMarker = str_find(pLine, ": id=");
	const char *pMsg = nullptr;
	if(pIdMarker)
		pMsg = pIdMarker + 2;
	else if(str_startswith(pLine, "id=") != nullptr)
		pMsg = pLine;
	else
		return false;

	const char *pIdStart = pMsg + 3;
	int Id = 0;
	const char *pCursor = pIdStart;
	while(*pCursor >= '0' && *pCursor <= '9')
	{
		Id = Id * 10 + (*pCursor - '0');
		pCursor++;
	}
	if(pCursor == pIdStart || *pCursor != ' ')
		return false;

	pOut->m_Id = Id;
	pOut->m_aName[0] = '\0';
	// Store the raw line verbatim (timestamp + "I server:" prefix + id=... payload) so
	// the clipboard copy includes everything the moderator needs for a ban post.
	str_copy(pOut->m_aFullLine, pLine);

	const char *pNameStart = str_find(pMsg, "name='");
	if(pNameStart)
	{
		pNameStart += 6;
		const char *pNameEnd = str_find(pNameStart, "' ");
		if(!pNameEnd)
		{
			const size_t Len = strlen(pNameStart);
			if(Len > 0 && pNameStart[Len - 1] == '\'')
				pNameEnd = pNameStart + Len - 1;
		}
		if(pNameEnd && pNameEnd > pNameStart)
		{
			const size_t NameLen = (size_t)(pNameEnd - pNameStart);
			const size_t Copy = NameLen < sizeof(pOut->m_aName) - 1 ? NameLen : sizeof(pOut->m_aName) - 1;
			memcpy(pOut->m_aName, pNameStart, Copy);
			pOut->m_aName[Copy] = '\0';
		}
	}
	return true;
}

void CModeratorPanel::OnRconLine(const char *pLine)
{
	CPlayerEntry Entry;
	if(!ParseStatusLine(pLine, &Entry))
		return;

	if(Entry.m_Id == 0)
		m_vPlayers.clear();

	for(auto &P : m_vPlayers)
	{
		if(P.m_Id == Entry.m_Id)
		{
			P = Entry;
			GameClient()->m_ChatWatcher.UpdateStatusLine(Entry.m_Id, Entry.m_aName, Entry.m_aFullLine);
			UpdateStoredEntry(Entry);
			return;
		}
	}
	m_vPlayers.push_back(Entry);
	GameClient()->m_ChatWatcher.UpdateStatusLine(Entry.m_Id, Entry.m_aName, Entry.m_aFullLine);
	UpdateStoredEntry(Entry);
}

void CModeratorPanel::TryLoadTodoJson()
{
	// Cheap stat first — only actually read + parse when the bridge has written a new file.
	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "todo_players.json", aPath, sizeof(aPath));
	time_t Created = 0, Modified = 0;
	if(fs_file_time(aPath, &Created, &Modified) != 0)
		return;
	const int64_t Mtime = (int64_t)Modified;
	if(Mtime == m_LastJsonMtime)
		return;
	m_LastJsonMtime = Mtime;

	// Read todo_players.json from the Moderator Client user dir.
	void *pBuf = nullptr;
	unsigned Length = 0;
	if(!Storage()->ReadFile("todo_players.json", IStorage::TYPE_SAVE, &pBuf, &Length))
		return;

	json_settings Settings{};
	char aError[256];
	json_value *pRoot = json_parse_ex(&Settings, static_cast<json_char *>(pBuf), Length, aError);
	free(pBuf);
	if(!pRoot)
		return;

	if(pRoot->type != json_object)
	{
		json_value_free(pRoot);
		return;
	}

	const json_value &Todos = (*pRoot)["todos"];
	if(Todos.type != json_array)
	{
		json_value_free(pRoot);
		return;
	}

	m_vTodoEntries.clear();
	m_vTodos.clear();

	for(unsigned i = 0; i < Todos.u.array.length; i++)
	{
		const json_value &Item = *Todos.u.array.values[i];
		if(Item.type != json_object)
			continue;

		CTodoEntry Entry;
		const json_value &Name = Item["name"];
		const json_value &Reason = Item["reason"];
		const json_value &PostUrl = Item["post_url"];
		const json_value &PostId = Item["post_id"];
		const json_value &FirstMsgAt = Item["first_message_at"];
		if(Name.type == json_string)
			Entry.m_Name.assign(Name.u.string.ptr, Name.u.string.length);
		if(Reason.type == json_string)
			Entry.m_Reason.assign(Reason.u.string.ptr, Reason.u.string.length);
		if(PostUrl.type == json_string)
			Entry.m_PostUrl.assign(PostUrl.u.string.ptr, PostUrl.u.string.length);
		if(PostId.type == json_string)
			Entry.m_PostId.assign(PostId.u.string.ptr, PostId.u.string.length);
		if(FirstMsgAt.type == json_string && FirstMsgAt.u.string.length > 0)
			Entry.m_FirstMessageAt = (int64_t)strtoll(FirstMsgAt.u.string.ptr, nullptr, 10);
		else if(FirstMsgAt.type == json_integer)
			Entry.m_FirstMessageAt = (int64_t)FirstMsgAt.u.integer;

		const json_value &Attachments = Item["attachments"];
		if(Attachments.type == json_array)
		{
			for(unsigned j = 0; j < Attachments.u.array.length; j++)
			{
				const json_value &A = *Attachments.u.array.values[j];
				if(A.type != json_object)
					continue;
				CAttachment Att;
				const json_value &Url = A["url"];
				const json_value &Filename = A["filename"];
				if(Url.type == json_string)
					Att.m_Url.assign(Url.u.string.ptr, Url.u.string.length);
				if(Filename.type == json_string)
					Att.m_Filename.assign(Filename.u.string.ptr, Filename.u.string.length);
				if(!Att.m_Url.empty())
					Entry.m_vAttachments.push_back(std::move(Att));
			}
		}

		if(!Entry.m_Name.empty())
		{
			m_vTodos.push_back(Entry.m_Name);
			m_vTodoEntries.push_back(std::move(Entry));
		}
	}

	// Remove stored entries whose names are no longer in the todo list
	// (Discord confirmed they are done).
	const size_t StoredSizeBefore = m_vStoredEntries.size();
	m_vStoredEntries.erase(
		std::remove_if(m_vStoredEntries.begin(), m_vStoredEntries.end(),
			[this](const CStoredEntry &E) { return !IsTodo(E.m_Name.c_str()); }),
		m_vStoredEntries.end());
	if(m_vStoredEntries.size() != StoredSizeBefore)
		m_StoredEntriesDirty = true;

	json_value_free(pRoot);
}

void CModeratorPanel::CopyBanLine(int PlayerId)
{
	for(const auto &P : m_vPlayers)
	{
		if(P.m_Id == PlayerId)
		{
			Input()->SetClipboardText(P.m_aFullLine);
			char aDbg[64];
			str_format(aDbg, sizeof(aDbg), "copied status line for id=%d", PlayerId);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", aDbg);
			return;
		}
	}
}

void CModeratorPanel::RenderDetectionLog(CUIRect Body)
{
	CUIRect Content, Footer;
	Body.Margin(8.0f, &Content);
	Content.HSplitBottom(28.0f, &Content, &Footer);

	const auto &vLog = GameClient()->m_ChatWatcher.Log();

	CUIRect HintRow;
	Content.HSplitTop(14.0f, &HintRow, &Content);
	TextRender()->TextColor(0.8f, 0.8f, 0.8f, 1.0f);
	Ui()->DoLabel(&HintRow,
		"Most recent first. Click [Copy] to copy sender's status line for /ban. Demo tick shown when recording.",
		10.0f, TEXTALIGN_ML);
	Content.HSplitTop(4.0f, nullptr, &Content);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	const float RowH = 40.0f;
	const int Num = (int)vLog.size();
	const float TotalH = Num * RowH;
	const float VisibleH = Content.h;
	const float MaxScroll = std::max(0.0f, TotalH - VisibleH);
	if(m_LogScrollY > MaxScroll) m_LogScrollY = MaxScroll;
	if(m_LogScrollY < 0.0f) m_LogScrollY = 0.0f;

	const vec2 Mouse = Ui()->MousePos();

	for(int i = Num - 1, Visible = 0; i >= 0; --i, ++Visible)
	{
		const auto &E = vLog[i];
		const float RowY = Content.y + Visible * RowH - m_LogScrollY;
		if(RowY + RowH < Content.y) continue;
		if(RowY > Content.y + Content.h) break;

		CUIRect Row = {Content.x, RowY, Content.w, RowH - 2.0f};
		Row.Draw(ColorRGBA(0.12f, 0.04f, 0.04f, 0.70f), IGraphics::CORNER_ALL, 4.0f);

		CUIRect Inner;
		Row.Margin(4.0f, &Inner);

		CUIRect BtnCol, XBtn;
		Inner.VSplitRight(22.0f, &Inner, &XBtn);
		Inner.VSplitRight(4.0f, &Inner, nullptr);
		Inner.VSplitRight(68.0f, &Inner, &BtnCol);
		Inner.VSplitRight(4.0f, &Inner, nullptr);

		CUIRect Top, Bottom;
		Inner.HSplitMid(&Top, &Bottom, 0.0f);

		// Top row: [HH:MM] sender (id) · matched=<word> · demo tick=<n> or (no demo)
		char aTime[16];
		{
			time_t Ts = (time_t)E.m_Timestamp;
			struct tm Tm;
#if defined(_WIN32)
			localtime_s(&Tm, &Ts);
#else
			localtime_r(&Ts, &Tm);
#endif
			str_format(aTime, sizeof(aTime), "%02d:%02d", Tm.tm_hour, Tm.tm_min);
		}
		// Extract just the demo filename (not the full path) so the hint is scannable.
		const char *pDemoShort = E.m_DemoFile.c_str();
		{
			const char *pSlash = strrchr(pDemoShort, '/');
			const char *pBack = strrchr(pDemoShort, '\\');
			if(pBack && pBack > pSlash) pSlash = pBack;
			if(pSlash) pDemoShort = pSlash + 1;
		}
		const char *pMap = E.m_MapName.empty() ? "(unknown map)" : E.m_MapName.c_str();
		char aTopLine[512];
		if(!E.m_DemoFile.empty() && E.m_DemoTick >= 0)
		{
			str_format(aTopLine, sizeof(aTopLine),
				"[%s] %s (id=%d)  ·  matched: %s  ·  map: %s  ·  demo: %s  tick=%d",
				aTime, E.m_SenderName.c_str(), E.m_SenderId,
				E.m_MatchedWord.c_str(), pMap, pDemoShort, E.m_DemoTick);
		}
		else
		{
			str_format(aTopLine, sizeof(aTopLine),
				"[%s] %s (id=%d)  ·  matched: %s  ·  map: %s  ·  (no demo recording)",
				aTime, E.m_SenderName.c_str(), E.m_SenderId,
				E.m_MatchedWord.c_str(), pMap);
		}
		TextRender()->TextColor(1.0f, 0.85f, 0.85f, 1.0f);
		Ui()->DoLabel(&Top, aTopLine, 11.0f, TEXTALIGN_ML);

		// Bottom: the message itself (quoted, white).
		char aMsgLine[384];
		str_format(aMsgLine, sizeof(aMsgLine), "\"%s\"", E.m_Message.c_str());
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		Ui()->DoLabel(&Bottom, aMsgLine, 11.0f, TEXTALIGN_ML);

		// Copy-status button on the right.
		const bool BtnHot = Mouse.x >= BtnCol.x && Mouse.x <= BtnCol.x + BtnCol.w &&
				    Mouse.y >= BtnCol.y && Mouse.y <= BtnCol.y + BtnCol.h;
		BtnCol.Draw(BtnHot ? ColorRGBA(0.3f, 0.5f, 0.9f, 0.95f) : ColorRGBA(0.2f, 0.35f, 0.7f, 0.85f),
			IGraphics::CORNER_ALL, 4.0f);
		Ui()->DoLabel(&BtnCol, "Copy", 11.0f, TEXTALIGN_MC);
		CClickable Click;
		Click.m_X = BtnCol.x; Click.m_Y = BtnCol.y; Click.m_W = BtnCol.w; Click.m_H = BtnCol.h;
		Click.m_Action = 3; Click.m_Data = i;
		m_vClickables.push_back(Click);

		// Per-row X to delete a single false positive.
		const bool XHot = Mouse.x >= XBtn.x && Mouse.x <= XBtn.x + XBtn.w &&
				  Mouse.y >= XBtn.y && Mouse.y <= XBtn.y + XBtn.h;
		XBtn.Draw(XHot ? ColorRGBA(0.75f, 0.15f, 0.15f, 0.95f) : ColorRGBA(0.35f, 0.08f, 0.08f, 0.85f),
			IGraphics::CORNER_ALL, 4.0f);
		Ui()->DoLabel(&XBtn, "x", 12.0f, TEXTALIGN_MC);
		CClickable XClick;
		XClick.m_X = XBtn.x; XClick.m_Y = XBtn.y; XClick.m_W = XBtn.w; XClick.m_H = XBtn.h;
		XClick.m_Action = 5; XClick.m_Data = i;
		m_vClickables.push_back(XClick);
	}
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	// Footer: clear log button.
	Footer.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f), IGraphics::CORNER_ALL, 4.0f);
	CUIRect FooterInner;
	Footer.Margin(4.0f, &FooterInner);
	CUIRect ClearBtn, EmptyHint;
	FooterInner.VSplitLeft(110.0f, &ClearBtn, &EmptyHint);
	FooterInner.VSplitLeft(118.0f, nullptr, &EmptyHint);

	const bool ClearHot = Mouse.x >= ClearBtn.x && Mouse.x <= ClearBtn.x + ClearBtn.w &&
			      Mouse.y >= ClearBtn.y && Mouse.y <= ClearBtn.y + ClearBtn.h;
	ClearBtn.Draw(ClearHot ? ColorRGBA(0.6f, 0.15f, 0.15f, 0.95f) : ColorRGBA(0.4f, 0.1f, 0.1f, 0.85f),
		IGraphics::CORNER_ALL, 4.0f);
	Ui()->DoLabel(&ClearBtn, "Clear log", 11.0f, TEXTALIGN_MC);
	CClickable ClearClick;
	ClearClick.m_X = ClearBtn.x; ClearClick.m_Y = ClearBtn.y;
	ClearClick.m_W = ClearBtn.w; ClearClick.m_H = ClearBtn.h;
	ClearClick.m_Action = 4; ClearClick.m_Data = 0;
	m_vClickables.push_back(ClearClick);

	char aHint[128];
	if(Num == 0)
		str_copy(aHint, "(log empty — detections will appear here)");
	else
		str_format(aHint, sizeof(aHint), "%d entries — scroll with mouse wheel", Num);
	TextRender()->TextColor(0.7f, 0.7f, 0.7f, 1.0f);
	Ui()->DoLabel(&EmptyHint, aHint, 10.0f, TEXTALIGN_ML);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

void CModeratorPanel::CopyDetectionStatusLine(int LogIndex)
{
	const auto &vLog = GameClient()->m_ChatWatcher.Log();
	if(LogIndex < 0 || LogIndex >= (int)vLog.size())
		return;
	const auto &E = vLog[LogIndex];

	// Prefer the live status line if the sender is still on the server.
	for(const auto &P : m_vPlayers)
	{
		if(P.m_Id == E.m_SenderId && str_comp(P.m_aName, E.m_SenderName.c_str()) == 0)
		{
			Input()->SetClipboardText(P.m_aFullLine);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", "copied live status line from detection");
			return;
		}
	}

	// Use the full status line captured at detection time if available.
	if(!E.m_SenderStatusLine.empty())
	{
		Input()->SetClipboardText(E.m_SenderStatusLine.c_str());
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", "copied stored status line from detection");
		return;
	}

	// Last resort: reconstruct what we have from the snapshot fields.
	char aLine[512];
	str_format(aLine, sizeof(aLine), "id=%d name='%s' clan='%s' country=%d",
		E.m_SenderId, E.m_SenderName.c_str(), E.m_SenderClan.c_str(), E.m_SenderCountry);
	Input()->SetClipboardText(aLine);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", "copied partial snapshot (no status line captured)");
}

void CModeratorPanel::SanitizeFilename(const char *pIn, char *pOut, size_t OutSize)
{
	// Replace chars that are invalid in Windows/Unix filenames with '_'.
	size_t o = 0;
	for(size_t i = 0; pIn[i] != '\0' && o + 1 < OutSize; i++)
	{
		const char c = pIn[i];
		const bool Bad = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
				 c == '\\' || c == '|' || c == '?' || c == '*' || (unsigned char)c < 32;
		pOut[o++] = Bad ? '_' : c;
	}
	if(o == 0)
	{
		str_copy(pOut, "_", OutSize);
		return;
	}
	pOut[o] = '\0';
}

void CModeratorPanel::BuildAttachmentPath(const char *pPlayerName, const char *pFilename, char *pOut, size_t OutSize) const
{
	char aSafeName[128];
	char aSafeFile[256];
	SanitizeFilename(pPlayerName, aSafeName, sizeof(aSafeName));
	SanitizeFilename(pFilename, aSafeFile, sizeof(aSafeFile));

	const char *pDir = g_Config.m_McAttachmentDir;
	if(pDir[0] != '\0')
	{
		str_format(pOut, OutSize, "%s/%s/%s", pDir, aSafeName, aSafeFile);
	}
	else
	{
		// Default: <userdir>/attachments/<player>/<file>
		char aAbsolute[IO_MAX_PATH_LENGTH];
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, "attachments", aAbsolute, sizeof(aAbsolute));
		str_format(pOut, OutSize, "%s/%s/%s", aAbsolute, aSafeName, aSafeFile);
	}
}

void CModeratorPanel::DownloadPlayerAttachments(const char *pPlayerName)
{
	const CTodoEntry *pTodo = FindTodo(pPlayerName);
	if(!pTodo)
		return;

	for(const auto &Att : pTodo->m_vAttachments)
	{
		CDownload D;
		D.m_PlayerName = pPlayerName;
		D.m_Filename = Att.m_Filename;
		// HttpGet returns unique_ptr — convert to shared so we can stash it and Http() can keep its own ref.
		std::unique_ptr<CHttpRequest> pReq = HttpGet(Att.m_Url.c_str());
		pReq->WriteToMemory();
		pReq->Timeout(CTimeout{15000, 0, 500, 10});
		pReq->LogProgress(HTTPLOG::FAILURE);
		D.m_pRequest = std::shared_ptr<CHttpRequest>(std::move(pReq));
		Http()->Run(D.m_pRequest);
		m_vDownloads.push_back(std::move(D));
	}
	char aDbg[128];
	str_format(aDbg, sizeof(aDbg), "queued %d download(s) for %s", (int)pTodo->m_vAttachments.size(), pPlayerName);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", aDbg);
}

void CModeratorPanel::MarkDone(int PlayerIndex)
{
	if(PlayerIndex < 0 || PlayerIndex >= (int)m_vPlayers.size())
		return;
	const char *pName = m_vPlayers[PlayerIndex].m_aName;
	const CTodoEntry *pTodo = FindTodo(pName);
	if(!pTodo || pTodo->m_PostId.empty())
		return;

	// Write a command file for the bridge to process (reply "Done" + add ✅ reaction).
	char aCmdName[128];
	str_format(aCmdName, sizeof(aCmdName), "todo_done_%s.json", pTodo->m_PostId.c_str());
	char aCmdPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, aCmdName, aCmdPath, sizeof(aCmdPath));
	char aJson[256];
	str_format(aJson, sizeof(aJson), "{\"action\":\"mark_done\",\"post_id\":\"%s\"}\n", pTodo->m_PostId.c_str());
	IOHANDLE f = io_open(aCmdPath, IOFLAG_WRITE);
	if(f)
	{
		io_write(f, aJson, (unsigned)str_length(aJson));
		io_close(f);
		char aDbg[256];
		str_format(aDbg, sizeof(aDbg), "marked done: %s (post_id=%s)", pName, pTodo->m_PostId.c_str());
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", aDbg);
	}

}

void CModeratorPanel::OpenEvidenceFolder()
{
	char aPath[IO_MAX_PATH_LENGTH];
	const char *pDir = g_Config.m_McAttachmentDir;
	if(pDir[0] != '\0')
		str_copy(aPath, pDir);
	else
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, "attachments", aPath, sizeof(aPath));
	fs_makedir(aPath);
	os_open_file(aPath);
}

void CModeratorPanel::PollDownloads()
{
	for(auto it = m_vDownloads.begin(); it != m_vDownloads.end();)
	{
		const EHttpState State = it->m_pRequest->State();
		if(State == EHttpState::QUEUED || State == EHttpState::RUNNING)
		{
			++it;
			continue;
		}

		if(State == EHttpState::DONE)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			BuildAttachmentPath(it->m_PlayerName.c_str(), it->m_Filename.c_str(), aPath, sizeof(aPath));

			// Ensure parent dir exists.
			char aDir[IO_MAX_PATH_LENGTH];
			str_copy(aDir, aPath);
			for(int i = str_length(aDir) - 1; i >= 0; i--)
			{
				if(aDir[i] == '/' || aDir[i] == '\\')
				{
					aDir[i] = '\0';
					break;
				}
			}
			fs_makedir_rec_for(aPath);
			fs_makedir(aDir);

			unsigned char *pBuf = nullptr;
			size_t Len = 0;
			it->m_pRequest->Result(&pBuf, &Len);
			if(pBuf && Len > 0)
			{
				IOHANDLE File = io_open(aPath, IOFLAG_WRITE);
				if(File)
				{
					io_write(File, pBuf, Len);
					io_close(File);
					char aDbg[IO_MAX_PATH_LENGTH + 32];
					str_format(aDbg, sizeof(aDbg), "saved %s", aPath);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", aDbg);
				}
				else
				{
					char aDbg[IO_MAX_PATH_LENGTH + 32];
					str_format(aDbg, sizeof(aDbg), "failed to open for write: %s", aPath);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", aDbg);
				}
			}
		}
		else
		{
			char aDbg[256];
			str_format(aDbg, sizeof(aDbg), "download failed for %s / %s", it->m_PlayerName.c_str(), it->m_Filename.c_str());
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mc_mod", aDbg);
		}

		it = m_vDownloads.erase(it);
	}
}

void CModeratorPanel::JsonEscapeStr(const char *pIn, char *pOut, size_t OutSize)
{
	size_t o = 0;
	for(size_t i = 0; pIn[i] != '\0' && o + 3 < OutSize; i++)
	{
		const unsigned char c = (unsigned char)pIn[i];
		if(c == '"' || c == '\\')
		{
			pOut[o++] = '\\';
			pOut[o++] = (char)c;
		}
		else if(c >= 0x20)
		{
			pOut[o++] = (char)c;
		}
	}
	pOut[o] = '\0';
}

void CModeratorPanel::SaveStoredEntries() const
{
	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "stored_targets.json", aPath, sizeof(aPath));
	IOHANDLE f = io_open(aPath, IOFLAG_WRITE);
	if(!f)
		return;

	io_write(f, "[", 1);
	bool First = true;
	for(const auto &E : m_vStoredEntries)
	{
		if(!First)
			io_write(f, ",", 1);
		First = false;

		char aName[256], aReason[1024], aStatus[2048];
		JsonEscapeStr(E.m_Name.c_str(), aName, sizeof(aName));
		JsonEscapeStr(E.m_Reason.c_str(), aReason, sizeof(aReason));
		JsonEscapeStr(E.m_FullStatusLine.c_str(), aStatus, sizeof(aStatus));

		char aBuf[4096];
		str_format(aBuf, sizeof(aBuf),
			"\n{\"name\":\"%s\",\"reason\":\"%s\",\"status\":\"%s\"}",
			aName, aReason, aStatus);
		io_write(f, aBuf, (unsigned)str_length(aBuf));
	}
	io_write(f, "\n]", 2);
	io_close(f);
}

void CModeratorPanel::LoadStoredEntries()
{
	void *pBuf = nullptr;
	unsigned Length = 0;
	if(!Storage()->ReadFile("stored_targets.json", IStorage::TYPE_SAVE, &pBuf, &Length))
		return;

	json_settings Settings{};
	char aError[256];
	json_value *pRoot = json_parse_ex(&Settings, static_cast<json_char *>(pBuf), Length, aError);
	free(pBuf);
	if(!pRoot || pRoot->type != json_array)
	{
		if(pRoot)
			json_value_free(pRoot);
		return;
	}

	m_vStoredEntries.clear();
	for(unsigned i = 0; i < pRoot->u.array.length; i++)
	{
		const json_value &Item = *pRoot->u.array.values[i];
		if(Item.type != json_object)
			continue;
		CStoredEntry E;
		const json_value &Name = Item["name"];
		const json_value &Reason = Item["reason"];
		const json_value &Status = Item["status"];
		if(Name.type == json_string)
			E.m_Name.assign(Name.u.string.ptr, Name.u.string.length);
		if(Reason.type == json_string)
			E.m_Reason.assign(Reason.u.string.ptr, Reason.u.string.length);
		if(Status.type == json_string)
			E.m_FullStatusLine.assign(Status.u.string.ptr, Status.u.string.length);
		if(!E.m_Name.empty())
			m_vStoredEntries.push_back(std::move(E));
	}
	json_value_free(pRoot);
}

void CModeratorPanel::UpdateStoredEntry(const CPlayerEntry &Entry)
{
	if(!IsTodo(Entry.m_aName))
		return;
	const CTodoEntry *pTodo = FindTodo(Entry.m_aName);

	for(auto &S : m_vStoredEntries)
	{
		if(S.m_Name == Entry.m_aName)
		{
			S.m_FullStatusLine = Entry.m_aFullLine;
			if(pTodo && S.m_Reason.empty() && !pTodo->m_Reason.empty())
				S.m_Reason = pTodo->m_Reason;
			m_StoredEntriesDirty = true;
			return;
		}
	}
	CStoredEntry S;
	S.m_Name = Entry.m_aName;
	S.m_FullStatusLine = Entry.m_aFullLine;
	if(pTodo)
		S.m_Reason = pTodo->m_Reason;
	m_vStoredEntries.push_back(std::move(S));
	m_StoredEntriesDirty = true;
}

void CModeratorPanel::RenderStoredLog(CUIRect Body)
{
	CUIRect Content, Footer;
	Body.Margin(8.0f, &Content);
	Content.HSplitBottom(28.0f, &Content, &Footer);

	CUIRect HintRow;
	Content.HSplitTop(14.0f, &HintRow, &Content);
	TextRender()->TextColor(0.8f, 0.8f, 0.8f, 1.0f);
	Ui()->DoLabel(&HintRow,
		"Players flagged by Discord. Stays until Discord confirms done. Click [Copy] to copy status line.",
		10.0f, TEXTALIGN_ML);
	Content.HSplitTop(4.0f, nullptr, &Content);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	const float RowH = 40.0f;
	const int Num = (int)m_vStoredEntries.size();
	const float TotalH = Num * RowH;
	const float VisibleH = Content.h;
	const float MaxScroll = std::max(0.0f, TotalH - VisibleH);
	if(m_LogScrollY > MaxScroll) m_LogScrollY = MaxScroll;
	if(m_LogScrollY < 0.0f) m_LogScrollY = 0.0f;

	const vec2 Mouse = Ui()->MousePos();

	for(int i = 0; i < Num; i++)
	{
		const CStoredEntry &E = m_vStoredEntries[i];
		const float RowY = Content.y + i * RowH - m_LogScrollY;
		if(RowY + RowH < Content.y) continue;
		if(RowY > Content.y + Content.h) break;

		CUIRect Row = {Content.x, RowY, Content.w, RowH - 2.0f};
		Row.Draw(ColorRGBA(0.04f, 0.04f, 0.14f, 0.70f), IGraphics::CORNER_ALL, 4.0f);

		CUIRect Inner;
		Row.Margin(4.0f, &Inner);

		CUIRect BtnCol;
		Inner.VSplitRight(68.0f, &Inner, &BtnCol);
		Inner.VSplitRight(4.0f, &Inner, nullptr);

		CUIRect Top, Bottom;
		Inner.HSplitMid(&Top, &Bottom, 0.0f);

		char aTopLine[256];
		if(!E.m_Reason.empty())
			str_format(aTopLine, sizeof(aTopLine), "%s  —  %s", E.m_Name.c_str(), E.m_Reason.c_str());
		else
			str_copy(aTopLine, E.m_Name.c_str());
		TextRender()->TextColor(1.0f, 0.75f, 0.4f, 1.0f);
		Ui()->DoLabel(&Top, aTopLine, 11.0f, TEXTALIGN_ML);

		const char *pStatus = E.m_FullStatusLine.empty()
			? "(no status line yet — player not yet seen on server)"
			: E.m_FullStatusLine.c_str();
		TextRender()->TextColor(0.85f, 0.85f, 0.85f, 1.0f);
		Ui()->DoLabel(&Bottom, pStatus, 10.0f, TEXTALIGN_ML);

		const bool BtnHot = Mouse.x >= BtnCol.x && Mouse.x <= BtnCol.x + BtnCol.w &&
				    Mouse.y >= BtnCol.y && Mouse.y <= BtnCol.y + BtnCol.h;
		BtnCol.Draw(BtnHot ? ColorRGBA(0.3f, 0.5f, 0.9f, 0.95f) : ColorRGBA(0.2f, 0.35f, 0.7f, 0.85f),
			IGraphics::CORNER_ALL, 4.0f);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		Ui()->DoLabel(&BtnCol, "Copy", 11.0f, TEXTALIGN_MC);
		CClickable Click;
		Click.m_X = BtnCol.x; Click.m_Y = BtnCol.y; Click.m_W = BtnCol.w; Click.m_H = BtnCol.h;
		Click.m_Action = 9; Click.m_Data = i;
		m_vClickables.push_back(Click);
	}
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	Footer.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f), IGraphics::CORNER_ALL, 4.0f);
	CUIRect FooterInner;
	Footer.Margin(4.0f, &FooterInner);
	char aHint[128];
	if(Num == 0)
		str_copy(aHint, "(no stored entries — targeted players appear here when seen on server)");
	else
		str_format(aHint, sizeof(aHint), "%d stored  —  scroll with mouse wheel", Num);
	TextRender()->TextColor(0.7f, 0.7f, 0.7f, 1.0f);
	Ui()->DoLabel(&FooterInner, aHint, 10.0f, TEXTALIGN_ML);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}

void CModeratorPanel::OnRender()
{
	// KoG server-name check (poll GetServerInfo until populated, run once per connect).
	if(!m_KogCheckDone && Client()->State() == IClient::STATE_ONLINE)
	{
		CServerInfo Info;
		Client()->GetServerInfo(&Info);
		if(Info.m_aName[0] != '\0')
		{
			m_KogCheckDone = true;
			if(str_startswith(Info.m_aName, "|*KoG*|") != nullptr ||
				str_startswith(Info.m_aName, "[A] |*KoG*|") != nullptr)
			{
				OnKogServerConnect();
			}
		}
	}

	// Auto-pull rcon status on scoreboard open (no throttle).
	const bool ScoreboardActive = GameClient()->m_Scoreboard.IsActive();
	if(ScoreboardActive && !m_WasScoreboardActive && Client()->RconAuthed())
		Client()->Rcon("status");
	m_WasScoreboardActive = ScoreboardActive;

	// Poll the Discord bridge's JSON every ~5s so scoreboard highlights stay fresh.
	const int64_t Now = time_get();
	if(Now - m_LastJsonLoadTick >= time_freq() * JSON_POLL_INTERVAL_SECONDS)
	{
		TryLoadTodoJson();
		m_LastJsonLoadTick = Now;
	}

	// Process finished attachment downloads.
	if(!m_vDownloads.empty())
		PollDownloads();

	const bool Active = IsActive();
	if(Active != m_WasActive)
	{
		if(Active)
		{
			if(!GameClient()->m_Menus.IsActive())
			{
				GameClient()->m_Menus.SetActive(true);
				m_OpenedMenus = true;
			}
			// Force an immediate JSON reload so the panel always shows fresh data on open.
			m_LastJsonLoadTick = 0;
		}
		else
		{
			if(m_OpenedMenus && GameClient()->m_Menus.IsActive())
				GameClient()->m_Menus.SetActive(false);
			m_OpenedMenus = false;
		}
		m_WasActive = Active;
	}

	if(!Active)
	{
		if(m_StoredEntriesDirty)
		{
			SaveStoredEntries();
			m_StoredEntriesDirty = false;
		}
		return;
	}

	// Persist stored entries while panel is open (debounced: 2 s after last change).
	if(m_StoredEntriesDirty && Now - m_LastStoredSaveTick >= time_freq() * 2)
	{
		SaveStoredEntries();
		m_StoredEntriesDirty = false;
		m_LastStoredSaveTick = Now;
	}

	// Use the UI's coord space so mouse positions match our stored rects.
	Ui()->MapScreen();
	const CUIRect *pScreen = Ui()->Screen();
	const float ScreenW = pScreen->w;
	const float ScreenH = pScreen->h;

	// Block the DDNet UI hot-item system so no background button (menus, scoreboard, etc.)
	// can become hot or active while the panel is open. SetHotItem with our unique id
	// overwrites any hot item set by components that rendered before us.
	Ui()->SetHotItem(&m_PanelX);

	CUIRect Screen = {0.0f, 0.0f, ScreenW, ScreenH};
	Screen.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.45f), IGraphics::CORNER_NONE, 0.0f);

	const float PanelW = std::min(820.0f, ScreenW - 80.0f);
	const float PanelH = std::min(480.0f, ScreenH - 80.0f);
	CUIRect Panel = {(ScreenW - PanelW) / 2.0f, (ScreenH - PanelH) / 2.0f, PanelW, PanelH};
	Panel.Draw(ColorRGBA(0.05f, 0.05f, 0.05f, 0.94f), IGraphics::CORNER_ALL, 12.0f);

	// Cache panel bounds for OnInput click-consume.
	m_PanelX = Panel.x; m_PanelY = Panel.y; m_PanelW = Panel.w; m_PanelH = Panel.h;

	CUIRect Header, Body;
	Panel.HSplitTop(38.0f, &Header, &Body);
	Header.Draw(ColorRGBA(0.92f, 0.29f, 0.48f, 0.95f), IGraphics::CORNER_T, 12.0f);

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	Ui()->DoLabel(&Header, "Moderator Panel", 22.0f, TEXTALIGN_MC);

	m_vClickables.clear();

	// Tab strip between header and content.
	CUIRect TabStrip;
	Body.HSplitTop(26.0f, &TabStrip, &Body);
	TabStrip.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f), IGraphics::CORNER_NONE, 0.0f);
	{
		char aTab0[64], aTab1[64], aTab2[64];
		str_format(aTab0, sizeof(aTab0), "Players (%d)", (int)m_vPlayers.size());
		str_format(aTab1, sizeof(aTab1), "Detection Log (%d)", (int)GameClient()->m_ChatWatcher.Log().size());
		str_format(aTab2, sizeof(aTab2), "Stored (%d)", (int)m_vStoredEntries.size());
		const char *apDynLabels[3] = {aTab0, aTab1, aTab2};
		const vec2 Mouse = Ui()->MousePos();
		const float TabW = TabStrip.w / 3.0f;
		for(int t = 0; t < 3; ++t)
		{
			CUIRect Tab = {TabStrip.x + t * TabW, TabStrip.y, TabW, TabStrip.h};
			const bool Active = (m_ActiveTab == t);
			const bool Hot = Mouse.x >= Tab.x && Mouse.x <= Tab.x + Tab.w &&
					 Mouse.y >= Tab.y && Mouse.y <= Tab.y + Tab.h;
			ColorRGBA Color = Active
				? ColorRGBA(0.92f, 0.29f, 0.48f, 0.55f)
				: (Hot ? ColorRGBA(1.0f, 1.0f, 1.0f, 0.12f) : ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f));
			Tab.Draw(Color, IGraphics::CORNER_NONE, 0.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, Active ? 1.0f : 0.75f);
			Ui()->DoLabel(&Tab, apDynLabels[t], 13.0f, TEXTALIGN_MC);
			CClickable Click;
			Click.m_X = Tab.x; Click.m_Y = Tab.y; Click.m_W = Tab.w; Click.m_H = Tab.h;
			Click.m_Action = 2; Click.m_Data = t;
			m_vClickables.push_back(Click);
		}
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	if(m_ActiveTab == 1)
	{
		RenderDetectionLog(Body);
		RenderTools()->RenderCursor(Ui()->MousePos(), 24.0f);
		return;
	}
	if(m_ActiveTab == 2)
	{
		RenderStoredLog(Body);
		RenderTools()->RenderCursor(Ui()->MousePos(), 24.0f);
		return;
	}

	CUIRect Content, Footer;
	Body.Margin(8.0f, &Content);
	Content.HSplitBottom(50.0f, &Content, &Footer);

	CUIRect CountRow;
	Content.HSplitTop(16.0f, &CountRow, &Content);
	char aCount[96];
	const int NumPlayers = (int)m_vPlayers.size();
	const int NumTodos = (int)m_vTodoEntries.size();
	str_format(aCount, sizeof(aCount), "%d player(s)  -  %d active to-do(s)  -  click name = copy ban line",
		NumPlayers, NumTodos);
	TextRender()->TextColor(0.8f, 0.8f, 0.8f, 1.0f);
	Ui()->DoLabel(&CountRow, aCount, 11.0f, TEXTALIGN_ML);
	Content.HSplitTop(6.0f, nullptr, &Content);

	// Two-column scrollable list. Click row to copy ban, click small download icon
	// (right edge, to-do only) to fetch attachments. Hit-testing done manually in
	// OnInput against m_vClickables since we render after menus' Ui frame closes.
	const float RowH = 18.0f;
	const float ColGap = 6.0f;
	const int NumCols = 2;
	const int NumRowsPerCol = (NumPlayers + NumCols - 1) / NumCols;
	const float TotalH = NumRowsPerCol * RowH;
	const float VisibleH = Content.h;

	const float MaxScroll = std::max(0.0f, TotalH - VisibleH);
	if(m_ScrollY > MaxScroll) m_ScrollY = MaxScroll;
	if(m_ScrollY < 0.0f) m_ScrollY = 0.0f;

	const float ColW = (Content.w - ColGap * (NumCols - 1)) / NumCols;
	const vec2 Mouse = Ui()->MousePos();

	Ui()->ClipEnable(&Content);
	for(size_t i = 0; i < m_vPlayers.size(); i++)
	{
		const CPlayerEntry &P = m_vPlayers[i];
		const int Col = (int)(i % NumCols);
		const int RowIdx = (int)(i / NumCols);
		const float RowY = Content.y + RowIdx * RowH - m_ScrollY;
		if(RowY + RowH < Content.y)
			continue; // above visible area
		if(RowY > Content.y + Content.h)
			continue; // below visible area

		CUIRect Row;
		Row.x = Content.x + Col * (ColW + ColGap);
		Row.y = RowY;
		Row.w = ColW;
		Row.h = RowH - 1.0f;

		const CTodoEntry *pTodo = FindTodo(P.m_aName);

		// Split: name area (left) + optional post/done/download buttons (right) for to-do rows.
		// Layout left-to-right: [ClickableName] [PostBtn] [DoneBtn] [DlIcon]
		CUIRect ClickableName = Row;
		CUIRect DlIcon = {0, 0, 0, 0};
		CUIRect DaysAgoRect = {0, 0, 0, 0};
		CUIRect DoneBtn = {0, 0, 0, 0};
		const bool HasAttach = pTodo && !pTodo->m_vAttachments.empty();
		const bool HasDone = pTodo && !pTodo->m_PostId.empty();
		if(HasAttach)
			ClickableName.VSplitRight(RowH, &ClickableName, &DlIcon);
		if(HasDone)
			ClickableName.VSplitRight(38.0f, &ClickableName, &DoneBtn);
		if(pTodo)
			ClickableName.VSplitRight(28.0f, &ClickableName, &DaysAgoRect);

		const bool NameHot =
			Mouse.x >= ClickableName.x && Mouse.x <= ClickableName.x + ClickableName.w &&
			Mouse.y >= ClickableName.y && Mouse.y <= ClickableName.y + ClickableName.h;
		const bool DlHot = HasAttach &&
			Mouse.x >= DlIcon.x && Mouse.x <= DlIcon.x + DlIcon.w &&
			Mouse.y >= DlIcon.y && Mouse.y <= DlIcon.y + DlIcon.h;
		const bool DoneHot = HasDone &&
			Mouse.x >= DoneBtn.x && Mouse.x <= DoneBtn.x + DoneBtn.w &&
			Mouse.y >= DoneBtn.y && Mouse.y <= DoneBtn.y + DoneBtn.h;

		// Background tint: red for to-do, subtle hover highlight otherwise.
		if(pTodo)
			ClickableName.Draw(NameHot ? ColorRGBA(0.55f, 0.10f, 0.10f, 0.90f) : ColorRGBA(0.25f, 0.05f, 0.05f, 0.55f),
				IGraphics::CORNER_ALL, 4.0f);
		else if(NameHot)
			ClickableName.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.12f), IGraphics::CORNER_ALL, 4.0f);

		// Render the name (and reason, elided) inside ClickableName.
		{
			CUIRect Text = ClickableName;
			Text.x += 6.0f;
			Text.w -= 12.0f;
			char aLine[192];
			if(pTodo && !pTodo->m_Reason.empty())
				str_format(aLine, sizeof(aLine), "[%d] %s  -  %s",
					P.m_Id, P.m_aName[0] ? P.m_aName : "(connecting)", pTodo->m_Reason.c_str());
			else
				str_format(aLine, sizeof(aLine), "[%d] %s",
					P.m_Id, P.m_aName[0] ? P.m_aName : "(connecting)");
			if(pTodo)
				TextRender()->TextColor(1.0f, 0.45f, 0.45f, 1.0f);
			else
				TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			Ui()->DoLabel(&Text, aLine, 12.0f, TEXTALIGN_ML);
		}

		// Store clickable rect so OnInput can hit-test on LMB.
		CClickable CopyAction;
		CopyAction.m_X = ClickableName.x; CopyAction.m_Y = ClickableName.y;
		CopyAction.m_W = ClickableName.w; CopyAction.m_H = ClickableName.h;
		CopyAction.m_Action = 0;
		CopyAction.m_Data = P.m_Id;
		m_vClickables.push_back(CopyAction);

		// Days-ago label: how many days since the first message of this todo post.
		if(pTodo)
		{
			char aDays[8];
			if(pTodo->m_FirstMessageAt > 0)
			{
				const int64_t NowSec = (int64_t)::time(nullptr);
				const int64_t DaysAgo = (NowSec - pTodo->m_FirstMessageAt) / 86400;
				str_format(aDays, sizeof(aDays), "%dd", (int)DaysAgo);
			}
			else
				str_copy(aDays, "?d");
			TextRender()->TextColor(0.55f, 0.55f, 0.55f, 1.0f);
			Ui()->DoLabel(&DaysAgoRect, aDays, 9.0f, TEXTALIGN_MC);
		}

		// Done button: replies "Done" to the forum post and reacts with ✅.
		if(HasDone)
		{
			DoneBtn.Draw(DoneHot ? ColorRGBA(0.15f, 0.65f, 0.25f, 0.95f) : ColorRGBA(0.1f, 0.42f, 0.18f, 0.85f),
				IGraphics::CORNER_ALL, 4.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			Ui()->DoLabel(&DoneBtn, "Done", 9.0f, TEXTALIGN_MC);

			CClickable DoneAction;
			DoneAction.m_X = DoneBtn.x; DoneAction.m_Y = DoneBtn.y;
			DoneAction.m_W = DoneBtn.w; DoneAction.m_H = DoneBtn.h;
			DoneAction.m_Action = 8;
			DoneAction.m_Data = (int)i;
			m_vClickables.push_back(DoneAction);
		}

		// Download icon (only on to-do rows with attachments).
		if(HasAttach)
		{
			DlIcon.Draw(DlHot ? ColorRGBA(0.3f, 0.5f, 0.9f, 0.95f) : ColorRGBA(0.2f, 0.35f, 0.7f, 0.85f),
				IGraphics::CORNER_ALL, 4.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			char aDl[8];
			str_format(aDl, sizeof(aDl), "%d", (int)pTodo->m_vAttachments.size());
			Ui()->DoLabel(&DlIcon, aDl, 11.0f, TEXTALIGN_MC);

			CClickable DlAction;
			DlAction.m_X = DlIcon.x; DlAction.m_Y = DlIcon.y;
			DlAction.m_W = DlIcon.w; DlAction.m_H = DlIcon.h;
			DlAction.m_Action = 1;
			DlAction.m_Data = (int)i;
			m_vClickables.push_back(DlAction);
		}
	}

	Ui()->ClipDisable();
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	// Footer: list of all active targeted (to-do) player names, so you know who to hunt
	// without scrolling the server browser.
	Footer.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.82f), IGraphics::CORNER_ALL, 4.0f);
	Ui()->ClipEnable(&Footer);
	CUIRect FooterInner;
	Footer.Margin(4.0f, &FooterInner);

	CUIRect FooterLabel;
	FooterInner.HSplitTop(12.0f, &FooterLabel, &FooterInner);

	// Open evidence folder button — always visible, regardless of targeted players.
	CUIRect FolderBtn;
	FooterLabel.VSplitRight(88.0f, &FooterLabel, &FolderBtn);
	const bool FolderHot = Mouse.x >= FolderBtn.x && Mouse.x <= FolderBtn.x + FolderBtn.w &&
				Mouse.y >= FolderBtn.y && Mouse.y <= FolderBtn.y + FolderBtn.h;
	FolderBtn.Draw(FolderHot ? ColorRGBA(0.5f, 0.38f, 0.1f, 0.95f) : ColorRGBA(0.35f, 0.26f, 0.06f, 0.85f),
		IGraphics::CORNER_ALL, 4.0f);
	TextRender()->TextColor(1.0f, 0.9f, 0.5f, 1.0f);
	Ui()->DoLabel(&FolderBtn, "Open Folder", 9.0f, TEXTALIGN_MC);
	CClickable FolderClick;
	FolderClick.m_X = FolderBtn.x; FolderClick.m_Y = FolderBtn.y;
	FolderClick.m_W = FolderBtn.w; FolderClick.m_H = FolderBtn.h;
	FolderClick.m_Action = 7; FolderClick.m_Data = 0;
	m_vClickables.push_back(FolderClick);

	TextRender()->TextColor(0.7f, 0.7f, 0.7f, 1.0f);
	Ui()->DoLabel(&FooterLabel, "Targeted players:", 9.5f, TEXTALIGN_ML);

	char aTargets[2048];
	aTargets[0] = '\0';
	for(size_t i = 0; i < m_vTodoEntries.size(); i++)
	{
		if(i > 0)
			str_append(aTargets, ", ");
		str_append(aTargets, m_vTodoEntries[i].m_Name.c_str());
	}
	if(aTargets[0] == '\0')
		str_copy(aTargets, "(none — bridge may still be loading)");
	TextRender()->TextColor(1.0f, 0.85f, 0.4f, 1.0f);
	{
		SLabelProperties Props;
		Props.m_MaxWidth = FooterInner.w;
		Props.m_EllipsisAtEnd = true;
		Ui()->DoLabel(&FooterInner, aTargets, 10.0f, TEXTALIGN_ML, Props);
	}
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	Ui()->ClipDisable();

	// Draw cursor last so it's on top of the panel.
	RenderTools()->RenderCursor(Ui()->MousePos(), 24.0f);
}
