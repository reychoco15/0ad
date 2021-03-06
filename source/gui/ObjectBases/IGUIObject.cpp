/* Copyright (C) 2021 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "IGUIObject.h"

#include "gui/CGUI.h"
#include "gui/CGUISetting.h"
#include "gui/Scripting/JSInterface_GUIProxy.h"
#include "js/Conversions.h"
#include "ps/CLogger.h"
#include "ps/GameSetup/Config.h"
#include "ps/Profile.h"
#include "scriptinterface/ScriptContext.h"
#include "scriptinterface/ScriptExtraHeaders.h"
#include "scriptinterface/ScriptInterface.h"
#include "soundmanager/ISoundManager.h"

#include <algorithm>
#include <unordered_map>

const CStr IGUIObject::EventNameMouseEnter = "MouseEnter";
const CStr IGUIObject::EventNameMouseMove = "MouseMove";
const CStr IGUIObject::EventNameMouseLeave = "MouseLeave";

IGUIObject::IGUIObject(CGUI& pGUI)
	: m_pGUI(pGUI),
	  m_pParent(),
	  m_MouseHovering(),
	  m_LastClickTime(),
	  m_Enabled(),
	  m_Hidden(),
	  m_Size(),
	  m_Style(),
	  m_Hotkey(),
	  m_Z(),
	  m_Absolute(),
	  m_Ghost(),
	  m_AspectRatio(),
	  m_Tooltip(),
	  m_TooltipStyle()
{
	RegisterSetting("enabled", m_Enabled);
	RegisterSetting("hidden", m_Hidden);
	RegisterSetting("size", m_Size);
	RegisterSetting("style", m_Style);
	RegisterSetting("hotkey", m_Hotkey);
	RegisterSetting("z", m_Z);
	RegisterSetting("absolute", m_Absolute);
	RegisterSetting("ghost", m_Ghost);
	RegisterSetting("aspectratio", m_AspectRatio);
	RegisterSetting("tooltip", m_Tooltip);
	RegisterSetting("tooltip_style", m_TooltipStyle);

	// Setup important defaults
	// TODO: Should be in the default style?
	SetSetting<bool>("hidden", false, true);
	SetSetting<bool>("ghost", false, true);
	SetSetting<bool>("enabled", true, true);
	SetSetting<bool>("absolute", true, true);
}

IGUIObject::~IGUIObject()
{
	for (const std::pair<const CStr, IGUISetting*>& p : m_Settings)
		delete p.second;

	if (!m_ScriptHandlers.empty())
		JS_RemoveExtraGCRootsTracer(m_pGUI.GetScriptInterface()->GetGeneralJSContext(), Trace, this);

	// m_Children is deleted along all other GUI Objects in the CGUI destructor
}

void IGUIObject::AddChild(IGUIObject& pChild)
{
	pChild.SetParent(this);
	m_Children.push_back(&pChild);
}

template<typename T>
void IGUIObject::RegisterSetting(const CStr& Name, T& Value)
{
	if (SettingExists(Name))
		LOGERROR("The setting '%s' already exists on the object '%s'!", Name.c_str(), GetPresentableName().c_str());
	else
		m_Settings.emplace(Name, new CGUISetting<T>(*this, Name, Value));
}

bool IGUIObject::SettingExists(const CStr& Setting) const
{
	return m_Settings.find(Setting) != m_Settings.end();
}

template <typename T>
T& IGUIObject::GetSetting(const CStr& Setting)
{
	return static_cast<CGUISetting<T>* >(m_Settings.at(Setting))->m_pSetting;
}

template <typename T>
const T& IGUIObject::GetSetting(const CStr& Setting) const
{
	return static_cast<CGUISetting<T>* >(m_Settings.at(Setting))->m_pSetting;
}

bool IGUIObject::SetSettingFromString(const CStr& Setting, const CStrW& Value, const bool SendMessage)
{
	const std::map<CStr, IGUISetting*>::iterator it = m_Settings.find(Setting);
	if (it == m_Settings.end())
	{
		LOGERROR("GUI object '%s' has no property called '%s', can't set parse and set value '%s'", GetPresentableName().c_str(), Setting.c_str(), Value.ToUTF8().c_str());
		return false;
	}
	return it->second->FromString(Value, SendMessage);
}

template <typename T>
void IGUIObject::SetSetting(const CStr& Setting, T& Value, const bool SendMessage)
{
	PreSettingChange(Setting);
	static_cast<CGUISetting<T>* >(m_Settings.at(Setting))->m_pSetting = std::move(Value);
	SettingChanged(Setting, SendMessage);
}

template <typename T>
void IGUIObject::SetSetting(const CStr& Setting, const T& Value, const bool SendMessage)
{
	PreSettingChange(Setting);
	static_cast<CGUISetting<T>* >(m_Settings.at(Setting))->m_pSetting = Value;
	SettingChanged(Setting, SendMessage);
}

void IGUIObject::PreSettingChange(const CStr& Setting)
{
	if (Setting == "hotkey")
		m_pGUI.UnsetObjectHotkey(this, GetSetting<CStr>(Setting));
}

void IGUIObject::SettingChanged(const CStr& Setting, const bool SendMessage)
{
	if (Setting == "size")
	{
		// If setting was "size", we need to re-cache itself and all children
		RecurseObject(nullptr, &IGUIObject::UpdateCachedSize);
	}
	else if (Setting == "hidden")
	{
		// Hiding an object requires us to reset it and all children
		if (m_Hidden)
			RecurseObject(nullptr, &IGUIObject::ResetStates);
	}
	else if (Setting == "hotkey")
		m_pGUI.SetObjectHotkey(this, GetSetting<CStr>(Setting));
	else if (Setting == "style")
		m_pGUI.SetObjectStyle(this, GetSetting<CStr>(Setting));

	if (SendMessage)
	{
		SGUIMessage msg(GUIM_SETTINGS_UPDATED, Setting);
		HandleMessage(msg);
	}
}

bool IGUIObject::IsMouseOver() const
{
	return m_CachedActualSize.PointInside(m_pGUI.GetMousePos());
}

bool IGUIObject::MouseOverIcon()
{
	return false;
}

void IGUIObject::UpdateMouseOver(IGUIObject* const& pMouseOver)
{
	if (pMouseOver == this)
	{
		if (!m_MouseHovering)
			SendMouseEvent(GUIM_MOUSE_ENTER,EventNameMouseEnter);

		m_MouseHovering = true;

		SendMouseEvent(GUIM_MOUSE_OVER, EventNameMouseMove);
	}
	else
	{
		if (m_MouseHovering)
		{
			m_MouseHovering = false;
			SendMouseEvent(GUIM_MOUSE_LEAVE, EventNameMouseLeave);
		}
	}
}

void IGUIObject::ChooseMouseOverAndClosest(IGUIObject*& pObject)
{
	if (!IsMouseOver())
		return;

	// Check if we've got competition at all
	if (pObject == nullptr)
	{
		pObject = this;
		return;
	}

	// Or if it's closer
	if (GetBufferedZ() >= pObject->GetBufferedZ())
	{
		pObject = this;
		return;
	}
}

IGUIObject* IGUIObject::GetParent() const
{
	// Important, we're not using GetParent() for these
	//  checks, that could screw it up
	if (m_pParent && m_pParent->m_pParent == nullptr)
		return nullptr;

	return m_pParent;
}

void IGUIObject::ResetStates()
{
	// Notify the gui that we aren't hovered anymore
	UpdateMouseOver(nullptr);
}

void IGUIObject::UpdateCachedSize()
{
	// If absolute="false" and the object has got a parent,
	//  use its cached size instead of the screen. Notice
	//  it must have just been cached for it to work.
	if (!m_Absolute && m_pParent && !IsRootObject())
		m_CachedActualSize = m_Size.GetSize(m_pParent->m_CachedActualSize);
	else
		m_CachedActualSize = m_Size.GetSize(CRect(0.f, 0.f, g_xres / g_GuiScale, g_yres / g_GuiScale));

	// In a few cases, GUI objects have to resize to fill the screen
	// but maintain a constant aspect ratio.
	// Adjust the size to be the max possible, centered in the original size:
	if (m_AspectRatio)
	{
		if (m_CachedActualSize.GetWidth() > m_CachedActualSize.GetHeight() * m_AspectRatio)
		{
			float delta = m_CachedActualSize.GetWidth() - m_CachedActualSize.GetHeight() * m_AspectRatio;
			m_CachedActualSize.left += delta/2.f;
			m_CachedActualSize.right -= delta/2.f;
		}
		else
		{
			float delta = m_CachedActualSize.GetHeight() - m_CachedActualSize.GetWidth() / m_AspectRatio;
			m_CachedActualSize.bottom -= delta/2.f;
			m_CachedActualSize.top += delta/2.f;
		}
	}
}

bool IGUIObject::ApplyStyle(const CStr& StyleName)
{
	if (!m_pGUI.HasStyle(StyleName))
	{
		LOGERROR("IGUIObject: Trying to use style '%s' that doesn't exist.", StyleName.c_str());
		return false;
	}

	// The default style may specify settings for any GUI object.
	// Other styles are reported if they specify a Setting that does not exist,
	// so that the XML author is informed and can correct the style.

	for (const std::pair<const CStr, CStrW>& p : m_pGUI.GetStyle(StyleName).m_SettingsDefaults)
	{
		if (SettingExists(p.first))
			SetSettingFromString(p.first, p.second, true);
		else if (StyleName != "default")
			LOGWARNING("GUI object has no setting \"%s\", but the style \"%s\" defines it", p.first, StyleName.c_str());
	}
	return true;
}

float IGUIObject::GetBufferedZ() const
{
	if (m_Absolute)
		return m_Z;

	if (GetParent())
		return GetParent()->GetBufferedZ() + m_Z;

	// In philosophy, a parentless object shouldn't be able to have a relative sizing,
	//  but we'll accept it so that absolute can be used as default without a complaint.
	//  Also, you could consider those objects children to the screen resolution.
	return m_Z;
}

void IGUIObject::RegisterScriptHandler(const CStr& eventName, const CStr& Code, CGUI& pGUI)
{
	ScriptRequest rq(pGUI.GetScriptInterface());

	const int paramCount = 1;
	const char* paramNames[paramCount] = { "mouse" };

	// Location to report errors from
	CStr CodeName = GetName() + " " + eventName;

	// Generate a unique name
	static int x = 0;
	char buf[64];
	sprintf_s(buf, ARRAY_SIZE(buf), "__eventhandler%d (%s)", x++, eventName.c_str());

	// TODO: this is essentially the same code as ScriptInterface::LoadScript (with a tweak for the argument).
	JS::CompileOptions options(rq.cx);
	options.setFileAndLine(CodeName.c_str(), 0);
	options.setIsRunOnce(false);

	JS::SourceText<mozilla::Utf8Unit> src;
	ENSURE(src.init(rq.cx, Code.c_str(), Code.length(), JS::SourceOwnership::Borrowed));
	JS::RootedObjectVector emptyScopeChain(rq.cx);
	JS::RootedFunction func(rq.cx, JS::CompileFunction(rq.cx, emptyScopeChain, options, buf, paramCount, paramNames, src));
	if (func == nullptr)
	{
		LOGERROR("RegisterScriptHandler: Failed to compile the script for %s", eventName.c_str());
		return;
	}

	JS::RootedObject funcObj(rq.cx, JS_GetFunctionObject(func));
	SetScriptHandler(eventName, funcObj);
}

void IGUIObject::SetScriptHandler(const CStr& eventName, JS::HandleObject Function)
{
	if (m_ScriptHandlers.empty())
		JS_AddExtraGCRootsTracer(m_pGUI.GetScriptInterface()->GetGeneralJSContext(), Trace, this);

	m_ScriptHandlers[eventName] = JS::Heap<JSObject*>(Function);

	if (std::find(m_pGUI.m_EventObjects[eventName].begin(), m_pGUI.m_EventObjects[eventName].end(), this) == m_pGUI.m_EventObjects[eventName].end())
		m_pGUI.m_EventObjects[eventName].emplace_back(this);
}

void IGUIObject::UnsetScriptHandler(const CStr& eventName)
{
	std::map<CStr, JS::Heap<JSObject*> >::iterator it = m_ScriptHandlers.find(eventName);

	if (it == m_ScriptHandlers.end())
		return;

	m_ScriptHandlers.erase(it);

	if (m_ScriptHandlers.empty())
		JS_RemoveExtraGCRootsTracer(m_pGUI.GetScriptInterface()->GetGeneralJSContext(), Trace, this);

	std::unordered_map<CStr, std::vector<IGUIObject*>>::iterator it2 = m_pGUI.m_EventObjects.find(eventName);
	if (it2 == m_pGUI.m_EventObjects.end())
		return;

	std::vector<IGUIObject*>& handlers = it2->second;
	handlers.erase(std::remove(handlers.begin(), handlers.end(), this), handlers.end());

	if (handlers.empty())
		m_pGUI.m_EventObjects.erase(it2);
}

InReaction IGUIObject::SendEvent(EGUIMessageType type, const CStr& eventName)
{
	PROFILE2_EVENT("gui event");
	PROFILE2_ATTR("type: %s", eventName.c_str());
	PROFILE2_ATTR("object: %s", m_Name.c_str());

	SGUIMessage msg(type);
	HandleMessage(msg);

	ScriptEvent(eventName);

	return msg.skipped ? IN_PASS : IN_HANDLED;
}

InReaction IGUIObject::SendMouseEvent(EGUIMessageType type, const CStr& eventName)
{
	PROFILE2_EVENT("gui mouse event");
	PROFILE2_ATTR("type: %s", eventName.c_str());
	PROFILE2_ATTR("object: %s", m_Name.c_str());

	SGUIMessage msg(type);
	HandleMessage(msg);

	ScriptRequest rq(m_pGUI.GetScriptInterface());

	// Set up the 'mouse' parameter
	JS::RootedValue mouse(rq.cx);

	const CPos& mousePos = m_pGUI.GetMousePos();

	ScriptInterface::CreateObject(
		rq,
		&mouse,
		"x", mousePos.x,
		"y", mousePos.y,
		"buttons", m_pGUI.GetMouseButtons());
	JS::RootedValueVector paramData(rq.cx);
	ignore_result(paramData.append(mouse));
	ScriptEvent(eventName, paramData);

	return msg.skipped ? IN_PASS : IN_HANDLED;
}

void IGUIObject::ScriptEvent(const CStr& eventName)
{
	ScriptEventWithReturn(eventName);
}

bool IGUIObject::ScriptEventWithReturn(const CStr& eventName)
{
	if (m_ScriptHandlers.find(eventName) == m_ScriptHandlers.end())
		return false;

	ScriptRequest rq(m_pGUI.GetScriptInterface());
	JS::RootedValueVector paramData(rq.cx);
	return ScriptEventWithReturn(eventName, paramData);
}

void IGUIObject::ScriptEvent(const CStr& eventName, const JS::HandleValueArray& paramData)
{
	ScriptEventWithReturn(eventName, paramData);
}

bool IGUIObject::ScriptEventWithReturn(const CStr& eventName, const JS::HandleValueArray& paramData)
{
	std::map<CStr, JS::Heap<JSObject*> >::iterator it = m_ScriptHandlers.find(eventName);
	if (it == m_ScriptHandlers.end())
		return false;

	ScriptRequest rq(m_pGUI.GetScriptInterface());
	JS::RootedObject obj(rq.cx, GetJSObject());
	JS::RootedValue handlerVal(rq.cx, JS::ObjectValue(*it->second));
	JS::RootedValue result(rq.cx);

	if (!JS_CallFunctionValue(rq.cx, obj, handlerVal, paramData, &result))
	{
		LOGERROR("Errors executing script event \"%s\"", eventName.c_str());
		ScriptException::CatchPending(rq);
		return false;
	}
	return JS::ToBoolean(result);
}

void IGUIObject::CreateJSObject()
{
	ScriptRequest rq(m_pGUI.GetScriptInterface());
	using ProxyHandler = JSI_GUIProxy<std::remove_pointer_t<decltype(this)>>;
	ProxyHandler::CreateJSObject(rq, this, GetGUI().GetProxyData(&ProxyHandler::Singleton()), m_JSObject);
}

JSObject* IGUIObject::GetJSObject()
{
	// Cache the object when somebody first asks for it, because otherwise
	// we end up doing far too much object allocation.
	if (!m_JSObject.initialized())
		CreateJSObject();

	return m_JSObject.get();
}

void IGUIObject::toString(ScriptInterface& scriptInterface, JS::MutableHandleValue ret)
{
	ScriptRequest rq(scriptInterface);
	ScriptInterface::ToJSVal(rq, ret, "[GUIObject: " + GetName() + "]");
}

void IGUIObject::focus(ScriptInterface& UNUSED(scriptInterface), JS::MutableHandleValue ret)
{
	GetGUI().SetFocusedObject(this);
	ret.setUndefined();
}

void IGUIObject::blur(ScriptInterface& UNUSED(scriptInterface), JS::MutableHandleValue ret)
{
	GetGUI().SetFocusedObject(nullptr);
	ret.setUndefined();
}

void IGUIObject::getComputedSize(ScriptInterface& scriptInterface, JS::MutableHandleValue ret)
{
	UpdateCachedSize();
	ScriptRequest rq(scriptInterface);
	ScriptInterface::ToJSVal(rq, ret, m_CachedActualSize);
}

bool IGUIObject::IsEnabled() const
{
	return m_Enabled;
}

bool IGUIObject::IsHidden() const
{
	return m_Hidden;
}

bool IGUIObject::IsHiddenOrGhost() const
{
	return m_Hidden || m_Ghost;
}

void IGUIObject::PlaySound(const CStrW& soundPath) const
{
	if (g_SoundManager && !soundPath.empty())
		g_SoundManager->PlayAsUI(soundPath.c_str(), false);
}

CStr IGUIObject::GetPresentableName() const
{
	// __internal(), must be at least 13 letters to be able to be
	//  an internal name
	if (m_Name.length() <= 12)
		return m_Name;

	if (m_Name.substr(0, 10) == "__internal")
		return CStr("[unnamed object]");
	else
		return m_Name;
}

void IGUIObject::SetFocus()
{
	m_pGUI.SetFocusedObject(this);
}

bool IGUIObject::IsFocused() const
{
	return m_pGUI.GetFocusedObject() == this;
}

bool IGUIObject::IsBaseObject() const
{
	return this == m_pGUI.GetBaseObject();
}

bool IGUIObject::IsRootObject() const
{
	return m_pParent == m_pGUI.GetBaseObject();
}

void IGUIObject::TraceMember(JSTracer* trc)
{
	// Please ensure to adapt the Tracer enabling and disabling in accordance with the GC things traced!

	for (std::pair<const CStr, JS::Heap<JSObject*>>& handler : m_ScriptHandlers)
		JS::TraceEdge(trc, &handler.second, "IGUIObject::m_ScriptHandlers");
}

// Instantiate templated functions:
// These functions avoid copies by working with a reference and move semantics.
#define TYPE(T) \
	template void IGUIObject::RegisterSetting<T>(const CStr& Name, T& Value); \
	template T& IGUIObject::GetSetting<T>(const CStr& Setting); \
	template const T& IGUIObject::GetSetting<T>(const CStr& Setting) const; \
	template void IGUIObject::SetSetting<T>(const CStr& Setting, T& Value, const bool SendMessage); \

#include "gui/GUISettingTypes.h"
#undef TYPE

// Copying functions - discouraged except for primitives.
#define TYPE(T) \
	template void IGUIObject::SetSetting<T>(const CStr& Setting, const T& Value, const bool SendMessage); \

#define GUITYPE_IGNORE_NONCOPYABLE
#include "gui/GUISettingTypes.h"
#undef GUITYPE_IGNORE_NONCOPYABLE
#undef TYPE
