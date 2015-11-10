/*
 * Module.cpp
 *
 *  Created on: Jun 24, 2015
 *      Author: gatanasov
 */
#include "Module.h"
#include "File.h"
#include "JniLocalRef.h"
#include "ArgConverter.h"
#include "V8GlobalHelpers.h"
#include "NativeScriptAssert.h"
#include "ExceptionUtil.h"
#include "Constants.h"

#include <sstream>
#include <assert.h>
#include <libgen.h>

using namespace v8;
using namespace std;
using namespace tns;

void Module::Init(Isolate *isolate)
{
	JEnv env;

	MODULE_CLASS = env.FindClass("com/tns/Module");
	assert(MODULE_CLASS != nullptr);

	GET_MODULE_PATH_METHOD_ID = env.GetStaticMethodID(MODULE_CLASS, "getModulePath", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
	assert(GET_MODULE_PATH_METHOD_ID != nullptr);

	string requireFactoryScript =
	"(function () { "
	"	function require_factory(requireInternal, dirName) { "
	"		return function require(modulePath) { "
	"			return requireInternal(modulePath, dirName); "
	"		} "
	"	} "
	"	return require_factory; "
	"})()";

	auto source = ConvertToV8String(requireFactoryScript);
	auto context = isolate->GetCurrentContext();

	Local<Script> script;
	auto maybeScript = Script::Compile(context, source).ToLocal(&script);

	assert(!script.IsEmpty());

	Local<Value> result;
	auto maybeResult = script->Run(context).ToLocal(&result);

	assert(!result.IsEmpty() && result->IsFunction());

	auto requireFactoryFunction = result.As<Function>();

	s_poRequireFactoryFunction = new Persistent<Function>(isolate, requireFactoryFunction);

	auto requireFuncTemplate = FunctionTemplate::New(isolate, RequireCallback);
	auto requireFunc = requireFuncTemplate->GetFunction();
	s_poRequireFunction = new Persistent<Function>(isolate, requireFunc);
}

Local<Function> Module::GetRequireFunction(Isolate *isolate, const string& dirName)
{
	Local<Function> requireFunc;

	auto itFound = s_requireCache.find(dirName);

	if (itFound != s_requireCache.end())
	{
		requireFunc = Local<Function>::New(isolate, *itFound->second);
	}
	else
	{
		auto requireFuncFactory = Local<Function>::New(isolate, *s_poRequireFactoryFunction);

		auto context = isolate->GetCurrentContext();

		auto requireInternalFunc = Local<Function>::New(isolate, *s_poRequireFunction);

		Local<Value> args[2] { requireInternalFunc, ConvertToV8String(dirName) };
		Local<Value> result;
		auto thiz = Object::New(isolate);
		auto success = requireFuncFactory->Call(context, thiz, 2, args).ToLocal(&result);

		assert(success && !result.IsEmpty() && result->IsFunction());

		requireFunc = result.As<Function>();

		auto poFunc = new Persistent<Function>(isolate, requireFunc);

		s_requireCache.insert(make_pair(dirName, poFunc));
	}

	return requireFunc;
}

void Module::RequireCallback(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto isolate = Isolate::GetCurrent();

	if (args.Length() != 2)
	{
		isolate->ThrowException(ConvertToV8String("require should be called with two parameters"));
		return;
	}
	if (!args[0]->IsString())
	{
		isolate->ThrowException(ConvertToV8String("require's first parameter should be string"));
		return;
	}
	if (!args[1]->IsString())
	{
		isolate->ThrowException(ConvertToV8String("require's second parameter should be string"));
		return;
	}

	string moduleName = ConvertToString(args[0].As<String>());
	string callingModuleDirName = ConvertToString(args[1].As<String>());

	JEnv env;
	JniLocalRef jsModulename(env.NewStringUTF(moduleName.c_str()));
	JniLocalRef jsCallingModuleDirName(env.NewStringUTF(callingModuleDirName.c_str()));
	JniLocalRef jsModulePath(env.CallStaticObjectMethod(MODULE_CLASS, GET_MODULE_PATH_METHOD_ID, (jstring) jsModulename, (jstring) jsCallingModuleDirName));

	// cache the required modules by full path, not name only, since there might be some collisions with relative paths and names
	string modulePath = ArgConverter::jstringToString((jstring) jsModulePath);
	if(modulePath == ""){
		// module not found
		stringstream ss;
		ss << "Module \"" << moduleName << "\" not found";
		string exception = ss.str();
		ExceptionUtil::GetInstance()->ThrowExceptionToJs(exception);
		return;
	}
	if (modulePath == "EXTERNAL_FILE_ERROR")
	{
		// module not found
		stringstream ss;
		ss << "Module \"" << moduleName << "\" is located on the external storage. Modules can be private application files ONLY";
		string exception = ss.str();
		ExceptionUtil::GetInstance()->ThrowExceptionToJs(exception);
		return;
	}

	auto it = s_loadedModules.find(modulePath);

	bool hasError = false;

	Local<Object> moduleObj;

	if (it == s_loadedModules.end())
	{
		moduleObj = CompileAndRun(modulePath, hasError);
	}
	else
	{
		moduleObj = Local<Object>::New(isolate, *it->second);
	}

	if(!hasError)
	{
		auto exportsObj = moduleObj->Get(ConvertToV8String("exports"));

		assert(!exportsObj.IsEmpty());

		args.GetReturnValue().Set(exportsObj);
	}
}

Local<Object> Module::CompileAndRun(const string& modulePath, bool& hasError)
{
	Local<Object> result;

	auto isolate = Isolate::GetCurrent();

	auto moduleObj = Object::New(isolate);
	auto exportsObj = Object::New(isolate);
	auto exportsPropName = ConvertToV8String("exports");
	moduleObj->Set(exportsPropName, exportsObj);

	auto poModuleObj = new Persistent<Object>(isolate, moduleObj);
	s_loadedModules.insert(make_pair(modulePath, poModuleObj));

	TryCatch tc;

	auto scriptText = Module::WrapModuleContent(modulePath);

	DEBUG_WRITE("Compiling script (module %s)", modulePath.c_str());
	auto fullRequiredModulePath = ConvertToV8String(modulePath);
	moduleObj->Set(ConvertToV8String("filename"), fullRequiredModulePath);
	//
	auto cacheData = TryLoadScriptCache(modulePath);

	ScriptOrigin origin(fullRequiredModulePath);
	ScriptCompiler::Source source(scriptText, origin, cacheData);
	ScriptCompiler::CompileOptions option = ScriptCompiler::kNoCompileOptions;
	Local<Script> script;

	if(cacheData != nullptr)
	{
		option = ScriptCompiler::kConsumeCodeCache;
		script = ScriptCompiler::Compile(isolate->GetCurrentContext(), &source, option).ToLocalChecked();
	}
	else
	{
		if(Constants::V8_CACHE_COMPILED_CODE)
		{
			option = ScriptCompiler::kProduceCodeCache;
		}
		script = ScriptCompiler::Compile(isolate->GetCurrentContext(), &source, option).ToLocalChecked();
		SaveScriptCache(source, modulePath);
	}
	//auto script = Script::Compile(scriptText, fullRequiredModulePath);
	//
	DEBUG_WRITE("Compiled script (module %s)", modulePath.c_str());

	if (ExceptionUtil::GetInstance()->HandleTryCatch(tc, "Script " + modulePath + " contains compilation errors!"))
	{
		hasError = true;
	}
	else if (script.IsEmpty())
	{
		//think about more descriptive message -> [script_name] was empty
		DEBUG_WRITE("%s was empty", modulePath.c_str());
	}
	else
	{
		DEBUG_WRITE("Running script (module %s)", modulePath.c_str());

		auto f = script->Run().As<Function>();
		if (ExceptionUtil::GetInstance()->HandleTryCatch(tc, "Error running script " + modulePath))
		{
			hasError = true;
		}
		else
		{
			auto fileName = ConvertToV8String(modulePath);
			char pathcopy[1024];
			strcpy(pathcopy, modulePath.c_str());
			string strDirName(dirname(pathcopy));
			auto dirName = ConvertToV8String(strDirName);
			auto require = GetRequireFunction(isolate, strDirName);
			Local<Value> requireArgs[5] { moduleObj, exportsObj, require, fileName, dirName };

			auto thiz = Object::New(isolate);
			f->Call(thiz, sizeof(requireArgs) / sizeof(Local<Value>), requireArgs);
			if(ExceptionUtil::GetInstance()->HandleTryCatch(tc, "Error calling module function "))
			{
				hasError = true;
			}
		}
	}

	if(hasError)
	{
		s_loadedModules.erase(modulePath);
		poModuleObj->Reset();
		delete poModuleObj;

		// this handles recursive require calls
		tc.ReThrow();
	}
	else
	{
		result = moduleObj;
	}

	return result;
}


Local<String> Module::WrapModuleContent(const string& path)
{
	string content = File::ReadText(path);

	auto separatorIndex = path.find_last_of("/");
	string dirName = path.substr(0, separatorIndex);

	// TODO: Use statically allocated buffer for better performance
	string result(MODULE_PART_1);
	result.reserve(content.length() + 1024);
	result += content;
	result += MODULE_PART_2;

	return ConvertToV8String(result);
}

ScriptCompiler::CachedData* Module::TryLoadScriptCache(const std::string& path)
{
	if(!Constants::V8_CACHE_COMPILED_CODE)
	{
		return nullptr;
	}

	auto cachePath = path + ".cache";
	if(!File::Exists(cachePath))
	{
		return nullptr;
	}

	auto file = fopen(cachePath.c_str(), "rb" /* read binary */);
	fseek(file, 0, SEEK_END);
	auto size = ftell(file);
	rewind(file);

	uint8_t* data = new uint8_t[size];
	fread(data, sizeof(uint8_t), size, file);

	auto cache = new ScriptCompiler::CachedData(data, size, ScriptCompiler::CachedData::BufferOwned);

	fclose(file);

	return cache;
}

void Module::SaveScriptCache(const ScriptCompiler::Source& source, const std::string& path)
{
	if(!Constants::V8_CACHE_COMPILED_CODE)
	{
		return;
	}

	int length = source.GetCachedData()->length;
	auto cachePath = path + ".cache";
	auto file = fopen(cachePath.c_str(), "wb" /* write binary */);
	fwrite(source.GetCachedData()->data, sizeof(uint8_t), length, file);
	fclose(file);
}


jclass Module::MODULE_CLASS = nullptr;
jmethodID Module::GET_MODULE_PATH_METHOD_ID = nullptr;

const char* Module::MODULE_PART_1 = "(function(module, exports, require, __filename, __dirname){ ";
const char* Module::MODULE_PART_2 = "\n})";
Persistent<Function>* Module::s_poRequireFunction = nullptr;
Persistent<Function>* Module::s_poRequireFactoryFunction = nullptr;
map<string, Persistent<Function>*> Module::s_requireCache;
map<string, Persistent<Object>*> Module::s_loadedModules;