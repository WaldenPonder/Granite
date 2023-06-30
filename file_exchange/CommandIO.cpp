#include "DataCenter/dtData.h"
#include "Common.h"
#include "CommandIO.h"
#include <QEventLoop>

begin_name_space(CommandIO);

struct CmdInfo
{
	io_type type;
	string			   prompt;
	int				   defaultInt;
	float			   defauleFloat;
	double			   defaultDouble;
	string			   defaultString;
	osg::Vec3		   defaultPos;
	osg::Vec2          defaultMousePos;
	vector<osg::Vec3>  defaultPosVec;
	dtData*			   defaultData;
	vector<dtData*>	defaultDatas;

	io_option							   option;
	string											   invalid_input_prompt;
	string											   pickClassName;
	vector<string>									   pickClassNames;
	std::function<bool(const string&)>				   isValidStringFun;
	QEventLoop										   loop;
	bool											   haveCommandProcess = false;
	bool                                               escPressed = false;
};

static CmdInfo cmd_info;

void clear_all_data()
{
	cmd_info.defaultPosVec.clear();
	cmd_info.defaultDatas.clear();
	cmd_info.defaultData = nullptr;
	cmd_info.pickClassNames.clear();
}

static void setInfoPara(const string& prompt, int intV,
						float floatV, double doubleV,
						io_option option,
						const string&		 invalid_input_prompt)
{
	msg::show_prompt m(prompt);
	msg::send(m);

	cmd_info.prompt				  = prompt;
	cmd_info.defaultInt			  = intV;
	cmd_info.defauleFloat		  = floatV;
	cmd_info.defaultDouble		  = doubleV;
	cmd_info.option				  = option;
	cmd_info.invalid_input_prompt = invalid_input_prompt;
}

void start()
{
	clear_all_data();

	cmd_info.haveCommandProcess = true;
	cmd_info.escPressed = false;

	LOG_INFO << cmd_info.loop.isRunning();

	if (cmd_info.loop.isRunning() == false)
	{
		cmd_info.loop.exec();
	}
}

void finish()
{
	cmd_info.loop.quit();

	msg::show_prompt m("命令:");
	msg::send(m);

	cmd_info.haveCommandProcess = false;
}

int getInt(const string& prompt, int default /*= 0*/, io_option option, const string& invalid_input_prompt)
{
	setInfoPara(prompt, default, 0, 0, option, invalid_input_prompt);
	cmd_info.type = eINT;

	start();

	//输入的值如果有效，会覆盖原有的defaultInt
	return cmd_info.defaultInt;
}

float getFloat(const string& prompt, float default /*= 0.f*/, io_option option, const string& invalid_input_prompt)
{
	setInfoPara(prompt, 0, default, 0., option, invalid_input_prompt);
	cmd_info.type = eFLOAT;

	start();

	return cmd_info.defauleFloat;
}

double getDouble(const string& prompt, double default /*= 0.*/, io_option option, const string& invalid_input_prompt)
{
	setInfoPara(prompt, 0, 0, default, option, invalid_input_prompt);
	cmd_info.type = eDOUBLE;

	start();

	return cmd_info.defaultDouble;
}

string getString(const string&							  prompt, const string& default,
							const std::function<bool(const string&)>& isValidStr,
							const string&							  invalid_input_prompt)
{
	setInfoPara(prompt, 0, 0, 0, io_option::eALLOW_EMPTY, invalid_input_prompt);
	cmd_info.type			  = eSTRING;
	cmd_info.defaultString	= default;
	cmd_info.isValidStringFun = isValidStr;
	start();

	return cmd_info.defaultString;
}

osg::Vec2 getMousePosition(const string& prompt, const string& invalid_input_prompt /*= ""*/)
{
	setInfoPara(prompt, 0, 0, 0, io_option::eALLOW_EMPTY, invalid_input_prompt);
	cmd_info.type = eMOUSE_POSITION;

	start();
	return cmd_info.defaultMousePos;
}

osg::Vec3 getPickPosition(const string& prompt, const string& invalid_input_prompt /*= ""*/)
{
	setInfoPara(prompt, 0, 0, 0, io_option::eALLOW_EMPTY, invalid_input_prompt);
	cmd_info.type = ePICK_POSITION;

	start();
	return cmd_info.defaultPos;
}

vector<osg::Vec3> getPickPositions(const string& prompt, const string& invalid_input_prompt)
{
	setInfoPara(prompt, 0, 0, 0, io_option::eALLOW_EMPTY, invalid_input_prompt);
	cmd_info.type = ePICK_POSITIONS;

	start();
	return cmd_info.defaultPosVec;
}

dtData* getPickSingle(const string& prompt, const string& className, const string& invalid_input_prompt)
{
	setInfoPara(prompt, 0, 0, 0, io_option::eALLOW_EMPTY, invalid_input_prompt);
	cmd_info.type		   = ePICK_SINGLE;
	cmd_info.pickClassName = className;

	start();
	return cmd_info.defaultData;
}

vector<dtData*> getPickObjects(const string& prompt, const vector<string>& classNames, const string& invalid_input_prompt)
{
	setInfoPara(prompt, 0, 0, 0, io_option::eALLOW_EMPTY, invalid_input_prompt);
	cmd_info.type			= ePICK_OBJECTS;
	cmd_info.pickClassNames = classNames;

	start();
	return cmd_info.defaultDatas;
}

bool haveCommandProcess()
{
	return cmd_info.haveCommandProcess;
}

bool haveEscapePressed()
{
	return cmd_info.escPressed;
}

io_type getType()
{
	return cmd_info.type;
}

void setText(const string& strPara)
{
	QString str = strPara.c_str();

	if (cmd_info.type == eINT)
	{
		if (str.isEmpty())
		{
			if (cmd_info.option == eALLOW_EMPTY)
			{
				finish();
			}
			else
			{
				msg::post(new msg::show_prompt("不允许为空，请重新输入"));
			}
		}
		else
		{
			bool bSuccess = false;
			int  val	  = str.toInt(&bSuccess);
			if (bSuccess)
			{
				cmd_info.defaultInt = val;
				finish();
			}
			else
			{
				msg::post(new msg::show_prompt(cmd_info.invalid_input_prompt));
			}
		}
	}
	else if (cmd_info.type == eFLOAT)
	{
		if (str.isEmpty())
		{
			if (cmd_info.option == eALLOW_EMPTY)
			{
				finish();
			}
			else
			{
				msg::post(new msg::show_prompt("不允许为空，请重新输入"));
			}
		}
		else
		{
			bool  bSuccess = false;
			float val	  = str.toFloat(&bSuccess);
			if (bSuccess)
			{
				cmd_info.defauleFloat = val;
				finish();
			}
			else
			{
				msg::post(new msg::show_prompt(cmd_info.invalid_input_prompt));
			}
		}
	}
	else if (cmd_info.type == eDOUBLE)
	{
		if (str.isEmpty())
		{
			if (cmd_info.option == eALLOW_EMPTY)
			{
				finish();
			}
			else
			{
				msg::post(new msg::show_prompt("不允许为空，请重新输入"));
			}
		}
		else
		{
			bool   bSuccess = false;
			double val		= str.toDouble(&bSuccess);
			if (bSuccess)
			{
				cmd_info.defaultDouble = val;
				finish();
			}
			else
			{
				msg::post(new msg::show_prompt(cmd_info.invalid_input_prompt));
			}
		}
	}
	else if (cmd_info.type == eSTRING)
	{
		if (cmd_info.isValidStringFun(strPara))
		{
			cmd_info.defaultString = strPara;
			finish();
		}
		else
		{
			msg::show_prompt m(cmd_info.invalid_input_prompt);
			msg::send(m);
		}
	}
}

void setMousePosition(const osg::Vec2& vec)
{
	if (cmd_info.type == eMOUSE_POSITION)
	{
		cmd_info.defaultMousePos = vec;
		finish();
	}
}

void setPosition(const osg::Vec3& vec)
{
	if (cmd_info.type == ePICK_POSITION)
	{
		cmd_info.defaultPos = vec;
		finish();
	}
}

void setPositions(const vector<osg::Vec3>& vec)
{
	if (cmd_info.type == ePICK_POSITIONS)
	{
		cmd_info.defaultPosVec = vec;
		finish();
	}
}

void setSelectedData(dtData* dt)
{
	if (cmd_info.type == ePICK_SINGLE)
	{
		if (cmd_info.pickClassName == "ALL" || cmd_info.pickClassName == dt->className())
		{
			cmd_info.defaultData = dt;
			finish();
		}
	}
}

void pushSelectedData(dtData* dt)
{
	if (!dt) return;

	if (cmd_info.type == ePICK_OBJECTS)
	{
		if (cmd_info.pickClassNames.size() == 0 || vector_exist_value(cmd_info.pickClassNames, dt->className()))
		{
			if (vector_exist_value(cmd_info.defaultDatas, dt) == false)
			{
				cmd_info.defaultDatas << dt;
			}			
		}
	}
}

void clearSelectedData()
{
	if (cmd_info.type == ePICK_OBJECTS)
	{
		cmd_info.defaultDatas.clear();
	}
}

void endSelectedDatas()
{
	if (cmd_info.defaultDatas.size())
	{
		finish();
	}
	else
	{
		msg::show_prompt m("选择数据不能为空， 请重新选择");
		msg::send(m);
	}
}

void setEscapePressed()
{
	clear_all_data();
	cmd_info.escPressed = true; finish();
}

end_name_space