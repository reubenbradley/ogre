/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreShaderPrecompiledHeaders.h"

namespace Ogre {

//-----------------------------------------------------------------------
template<> 
RTShader::ShaderGenerator* Singleton<RTShader::ShaderGenerator>::msSingleton = 0;

namespace RTShader {

/** Shader generator RenderObjectListener sub class. */
class SGRenderObjectListener : public RenderObjectListener, public RTShaderSystemAlloc
{
public:
    SGRenderObjectListener(ShaderGenerator* owner) { mOwner = owner; }

    /**
    Listener overridden function notify the shader generator when rendering single object.
    */
    virtual void notifyRenderSingleObject(Renderable* rend, const Pass* pass,
                                          const AutoParamDataSource* source, const LightList* pLightList,
                                          bool suppressRenderStateChanges)
    {
        mOwner->notifyRenderSingleObject(rend, pass, source, pLightList, suppressRenderStateChanges);
    }

protected:
    ShaderGenerator* mOwner;
};

/** Shader generator scene manager sub class. */
class SGSceneManagerListener : public SceneManager::Listener, public RTShaderSystemAlloc
{
public:
    SGSceneManagerListener(ShaderGenerator* owner) { mOwner = owner; }

    /**
    Listener overridden function notify the shader generator when finding visible objects process started.
    */
    virtual void preFindVisibleObjects(SceneManager* source, SceneManager::IlluminationRenderStage irs,
                                       Viewport* v)
    {
        mOwner->preFindVisibleObjects(source, irs, v);
    }
protected:
    // The shader generator instance.
    ShaderGenerator* mOwner;
};

/** Shader generator ScriptTranslatorManager sub class. */
class SGScriptTranslatorManager : public ScriptTranslatorManager
{
public:
    SGScriptTranslatorManager(ShaderGenerator* owner) { mOwner = owner; }

    /// Returns a manager for the given object abstract node, or null if it is not supported
    virtual ScriptTranslator* getTranslator(const AbstractNodePtr& node)
    {
        return mOwner->getTranslator(node);
    }

protected:
    // The shader generator instance.
    ShaderGenerator* mOwner;
};

class SGResourceGroupListener : public ResourceGroupListener
{
public:
    SGResourceGroupListener(ShaderGenerator* owner) { mOwner = owner; }

    /// sync our internal list if material gets dropped
    virtual void resourceRemove(const ResourcePtr& resource)
    {
        if (!dynamic_cast<Material*>(resource.get()))
            return;
        mOwner->removeAllShaderBasedTechniques(resource->getName(), resource->getGroup());
    }

protected:
    // The shader generator instance.
    ShaderGenerator* mOwner;
};

String ShaderGenerator::DEFAULT_SCHEME_NAME     = "ShaderGeneratorDefaultScheme";
String ShaderGenerator::SGTechnique::UserKey    = "SGTechnique";

//-----------------------------------------------------------------------
ShaderGenerator* ShaderGenerator::getSingletonPtr()
{
    return msSingleton;
}

//-----------------------------------------------------------------------
ShaderGenerator& ShaderGenerator::getSingleton()
{
    assert( msSingleton );  
    return ( *msSingleton );
}

//-----------------------------------------------------------------------------
ShaderGenerator::ShaderGenerator() :
    mActiveSceneMgr(NULL), mShaderLanguage(""),
    mFSLayer(0), mActiveViewportValid(false), mVSOutputCompactPolicy(VSOCP_LOW),
    mCreateShaderOverProgrammablePass(false), mIsFinalizing(false)
{
    mLightCount[0]              = 0;
    mLightCount[1]              = 0;
    mLightCount[2]              = 0;

    HighLevelGpuProgramManager& hmgr = HighLevelGpuProgramManager::getSingleton();

    if (hmgr.isLanguageSupported("glsles"))
    {
        mShaderLanguage = "glsles";
    }
    else if (hmgr.isLanguageSupported("glsl"))
    {
        mShaderLanguage = "glsl";
    }
    else if (hmgr.isLanguageSupported("hlsl"))
    {
        mShaderLanguage = "hlsl";
    }
    else if (hmgr.isLanguageSupported("cg"))
    {
        mShaderLanguage = "cg";
    }
    else
    {
        mShaderLanguage = "null"; // falling back to HLSL, for unit tests mainly
        LogManager::getSingleton().logWarning("ShaderGenerator: No supported language found. Falling back to 'null'");
    }

    setShaderProfiles(GPT_VERTEX_PROGRAM, "gpu_vp gp4vp vp40 vp30 arbvp1 vs_4_0 vs_4_0_level_9_3 "
                                          "vs_4_0_level_9_1 vs_3_0 vs_2_x vs_2_a vs_2_0 vs_1_1 glslv");
    setShaderProfiles(GPT_FRAGMENT_PROGRAM, "ps_4_0 ps_4_0_level_9_3 ps_4_0_level_9_1 ps_3_x ps_3_0 fp40 "
                                            "fp30 fp20 arbfp1 ps_2_x ps_2_a ps_2_b ps_2_0 ps_1_4 ps_1_3 "
                                            "ps_1_2 ps_1_1 glslf");
}

//-----------------------------------------------------------------------------
ShaderGenerator::~ShaderGenerator()
{
    
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::initialize()
{
    if (msSingleton == NULL)
    {
        msSingleton = OGRE_NEW ShaderGenerator;
        if (false == msSingleton->_initialize())
        {
            OGRE_DELETE msSingleton;
            msSingleton = NULL;
            return false;
        }
    }
        
    return true;
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::_initialize()
{
    OGRE_LOCK_AUTO_MUTEX;

    // Allocate program writer manager.
    mProgramWriterManager.reset(new ProgramWriterManager);

    // Allocate program manager.
    mProgramManager.reset(new ProgramManager);

    // Allocate and initialize FFP render state builder.
#ifdef RTSHADER_SYSTEM_BUILD_CORE_SHADERS
    mFFPRenderStateBuilder.reset(new FFPRenderStateBuilder);
#endif

    // Create extensions factories.
    createBuiltinSRSFactories();

    // Allocate script translator manager.
    mScriptTranslatorManager.reset(new SGScriptTranslatorManager(this));
    ScriptCompilerManager::getSingleton().addTranslatorManager(mScriptTranslatorManager.get());
    ID_RT_SHADER_SYSTEM = ScriptCompilerManager::getSingleton().registerCustomWordId("rtshader_system");

    // Create the default scheme.
    createScheme(DEFAULT_SCHEME_NAME);
	
	mResourceGroupListener.reset(new SGResourceGroupListener(this));
	ResourceGroupManager::getSingleton().addResourceGroupListener(mResourceGroupListener.get());

    return true;
}



//-----------------------------------------------------------------------------
void ShaderGenerator::createBuiltinSRSFactories()
{
    OGRE_LOCK_AUTO_MUTEX;
    SubRenderStateFactory* curFactory;
#ifdef RTSHADER_SYSTEM_BUILD_CORE_SHADERS
    curFactory = OGRE_NEW FFPTransformFactory;
    ShaderGenerator::getSingleton().addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);

    curFactory = OGRE_NEW FFPColourFactory;
    ShaderGenerator::getSingleton().addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);

    curFactory = OGRE_NEW FFPLightingFactory;
    ShaderGenerator::getSingleton().addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);

    curFactory = OGRE_NEW FFPTexturingFactory;
    ShaderGenerator::getSingleton().addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);

    curFactory = OGRE_NEW FFPFogFactory;
    ShaderGenerator::getSingleton().addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);

    curFactory = OGRE_NEW FFPAlphaTestFactory;
    ShaderGenerator::getSingleton().addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);
#endif
#ifdef RTSHADER_SYSTEM_BUILD_EXT_SHADERS
    // check if we are running an old shader level in d3d11
    bool d3d11AndLowProfile = ( (GpuProgramManager::getSingleton().isSyntaxSupported("vs_4_0_level_9_1") ||
        GpuProgramManager::getSingleton().isSyntaxSupported("vs_4_0_level_9_3"))
        && !GpuProgramManager::getSingleton().isSyntaxSupported("vs_4_0"));
    if(!d3d11AndLowProfile)
    {
        curFactory = OGRE_NEW PerPixelLightingFactory;  
        addSubRenderStateFactory(curFactory);
        mBuiltinSRSFactories.push_back(curFactory);

        curFactory = OGRE_NEW NormalMapLightingFactory; 
        addSubRenderStateFactory(curFactory);
        mBuiltinSRSFactories.push_back(curFactory);

        curFactory = OGRE_NEW IntegratedPSSM3Factory;   
        addSubRenderStateFactory(curFactory);
        mBuiltinSRSFactories.push_back(curFactory);

        curFactory = OGRE_NEW LayeredBlendingFactory;   
        addSubRenderStateFactory(curFactory);
        mBuiltinSRSFactories.push_back(curFactory);

        curFactory = OGRE_NEW HardwareSkinningFactory;  
        addSubRenderStateFactory(curFactory);
        mBuiltinSRSFactories.push_back(curFactory);
    }

    curFactory = OGRE_NEW TextureAtlasSamplerFactory;
    addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);
    
    curFactory = OGRE_NEW TriplanarTexturingFactory;
    addSubRenderStateFactory(curFactory);
    mBuiltinSRSFactories.push_back(curFactory);
#endif
}

//-----------------------------------------------------------------------------
void ShaderGenerator::destroy()
{
    if (msSingleton != NULL)
    {
        msSingleton->_destroy();

        OGRE_DELETE msSingleton;
        msSingleton = NULL;
    }
}

//-----------------------------------------------------------------------------
void ShaderGenerator::_destroy()
{
    OGRE_LOCK_AUTO_MUTEX;
    
    mIsFinalizing = true;
    
    // Delete technique entries.
    for (SGTechniqueMapIterator itTech = mTechniqueEntriesMap.begin(); itTech != mTechniqueEntriesMap.end(); ++itTech)
    {           
        OGRE_DELETE (itTech->second);
    }
    mTechniqueEntriesMap.clear();

    // Delete material entries.
    for (SGMaterialIterator itMat = mMaterialEntriesMap.begin(); itMat != mMaterialEntriesMap.end(); ++itMat)
    {       
        OGRE_DELETE (itMat->second);
    }
    mMaterialEntriesMap.clear();

    // Delete scheme entries.
    for (SGSchemeIterator itScheme = mSchemeEntriesMap.begin(); itScheme != mSchemeEntriesMap.end(); ++itScheme)
    {       
        OGRE_DELETE (itScheme->second);
    }
    mSchemeEntriesMap.clear();

    // Destroy extensions factories.
    destroyBuiltinSRSFactories();

#ifdef RTSHADER_SYSTEM_BUILD_CORE_SHADERS
    mFFPRenderStateBuilder.reset();
#endif

    mProgramManager.reset();
    mProgramWriterManager.reset();

    // Delete script translator manager.
    if (mScriptTranslatorManager)
    {
        ScriptCompilerManager::getSingleton().removeTranslatorManager(mScriptTranslatorManager.get());
        mScriptTranslatorManager.reset();
    }

    mMaterialSerializerListener.reset();

    if (mResourceGroupListener)
    {
        ResourceGroupManager::getSingleton().removeResourceGroupListener(mResourceGroupListener.get());
        mResourceGroupListener.reset();
    }

    // Remove all scene managers.   
    while (mSceneManagerMap.empty() == false)
    {
        SceneManagerIterator itSceneMgr    = mSceneManagerMap.begin();

        removeSceneManager(itSceneMgr->second);
    }

    mRenderObjectListener.reset();
    mSceneManagerListener.reset();
}

//-----------------------------------------------------------------------------
void ShaderGenerator::destroyBuiltinSRSFactories()
{
    OGRE_LOCK_AUTO_MUTEX;

    for (auto f : mBuiltinSRSFactories)
    {
        removeSubRenderStateFactory(f);
        delete f;
    }
    mBuiltinSRSFactories.clear();
}

//-----------------------------------------------------------------------------
void ShaderGenerator::addSubRenderStateFactory(SubRenderStateFactory* factory)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator itFind = mSubRenderStateFactories.find(factory->getType());

    if (itFind != mSubRenderStateFactories.end())
    {
        OGRE_EXCEPT(Exception::ERR_DUPLICATE_ITEM,
            "A factory of type '" + factory->getType() + "' already exists.",
            "ShaderGenerator::addSubRenderStateFactory");
    }       
    
    mSubRenderStateFactories[factory->getType()] = factory;
}

//-----------------------------------------------------------------------------
size_t ShaderGenerator::getNumSubRenderStateFactories() const
{
    return mSubRenderStateFactories.size();
}


//-----------------------------------------------------------------------------
SubRenderStateFactory*  ShaderGenerator::getSubRenderStateFactory(size_t index)
{
    {
        OGRE_LOCK_AUTO_MUTEX;

        SubRenderStateFactoryIterator itFind = mSubRenderStateFactories.begin();
        for(; index != 0 && itFind != mSubRenderStateFactories.end(); --index , ++itFind);

        if (itFind != mSubRenderStateFactories.end())
        {
            return itFind->second;
        }
    }

    OGRE_EXCEPT(Exception::ERR_DUPLICATE_ITEM,
        "A factory on index " + StringConverter::toString(index) + " does not exist.",
        "ShaderGenerator::addSubRenderStateFactory");
        
    return NULL;
}
//-----------------------------------------------------------------------------
SubRenderStateFactory* ShaderGenerator::getSubRenderStateFactory(const String& type)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator itFind = mSubRenderStateFactories.find(type);
    return (itFind != mSubRenderStateFactories.end()) ? itFind->second : NULL;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::removeSubRenderStateFactory(SubRenderStateFactory* factory)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator itFind = mSubRenderStateFactories.find(factory->getType());

    if (itFind != mSubRenderStateFactories.end())
        mSubRenderStateFactories.erase(itFind);

}

//-----------------------------------------------------------------------------
SubRenderState* ShaderGenerator::createSubRenderState(const String& type)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator itFind = mSubRenderStateFactories.find(type);

    if (itFind != mSubRenderStateFactories.end())
        return itFind->second->createInstance();


    OGRE_EXCEPT(Exception::ERR_ITEM_NOT_FOUND,
        "A factory of type '" + type + "' doesn't exists.",
        "ShaderGenerator::createSubRenderState");

    return NULL;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::destroySubRenderState(SubRenderState* subRenderState)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator itFind = mSubRenderStateFactories.find(subRenderState->getType());

    if (itFind != mSubRenderStateFactories.end())
    {
         itFind->second->destroyInstance(subRenderState);
    }   
}

//-----------------------------------------------------------------------------
SubRenderState* ShaderGenerator::createSubRenderState(ScriptCompiler* compiler, 
                                                      PropertyAbstractNode* prop, Pass* pass, SGScriptTranslator* translator)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator it = mSubRenderStateFactories.begin();
    SubRenderStateFactoryIterator itEnd = mSubRenderStateFactories.end();
    SubRenderState* subRenderState = NULL;

    while (it != itEnd)
    {
        subRenderState = it->second->createInstance(compiler, prop, pass, translator);
        if (subRenderState != NULL)     
            break;              
        ++it;
    }   

    return subRenderState;
}


//-----------------------------------------------------------------------------
SubRenderState* ShaderGenerator::createSubRenderState(ScriptCompiler* compiler, 
                                                      PropertyAbstractNode* prop, TextureUnitState* texState, SGScriptTranslator* translator)
{
    OGRE_LOCK_AUTO_MUTEX;

    SubRenderStateFactoryIterator it = mSubRenderStateFactories.begin();
    SubRenderStateFactoryIterator itEnd = mSubRenderStateFactories.end();
    SubRenderState* subRenderState = NULL;

    while (it != itEnd)
    {
        subRenderState = it->second->createInstance(compiler, prop, texState, translator);
        if (subRenderState != NULL)     
            break;              
        ++it;
    }   

    return subRenderState;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::createScheme(const String& schemeName)
{
    createOrRetrieveScheme(schemeName);
}

//-----------------------------------------------------------------------------
RenderState* ShaderGenerator::getRenderState(const String& schemeName)
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeIterator itFind = mSchemeEntriesMap.find(schemeName);
    
    if (itFind == mSchemeEntriesMap.end())
    {
        OGRE_EXCEPT(Exception::ERR_ITEM_NOT_FOUND,
            "A scheme named'" + schemeName + "' doesn't exists.",
            "ShaderGenerator::getRenderState"); 
    }   
    
    return itFind->second->getRenderState();
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::hasRenderState(const String& schemeName) const
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeConstIterator itFind = mSchemeEntriesMap.find(schemeName);
    return itFind != mSchemeEntriesMap.end();
}

//-----------------------------------------------------------------------------
ShaderGenerator::RenderStateCreateOrRetrieveResult ShaderGenerator::createOrRetrieveRenderState(const String& schemeName)
{
    SchemeCreateOrRetrieveResult res = createOrRetrieveScheme(schemeName);
    return RenderStateCreateOrRetrieveResult(res.first->getRenderState(),res.second);
}

//-----------------------------------------------------------------------------
ShaderGenerator::SchemeCreateOrRetrieveResult ShaderGenerator::createOrRetrieveScheme(const String& schemeName)
{
    OGRE_LOCK_AUTO_MUTEX;

    bool wasCreated = false;
    SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);
    SGScheme* schemeEntry = NULL;

    if (itScheme == mSchemeEntriesMap.end())
    {
        schemeEntry = OGRE_NEW SGScheme(schemeName);
        mSchemeEntriesMap.insert(SGSchemeMap::value_type(schemeName, schemeEntry));
        wasCreated = true;
    }
    else
    {
        schemeEntry = itScheme->second;
    }

    return SchemeCreateOrRetrieveResult(schemeEntry, wasCreated);
}
//-----------------------------------------------------------------------------
RenderState* ShaderGenerator::getRenderState(const String& schemeName, 
                                     const String& materialName, 
                                     const String& groupName, 
                                     unsigned short passIndex)
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeIterator itFind = mSchemeEntriesMap.find(schemeName);

    if (itFind == mSchemeEntriesMap.end())
    {
        OGRE_EXCEPT(Exception::ERR_ITEM_NOT_FOUND,
            "A scheme named'" + schemeName + "' doesn't exists.",
            "ShaderGenerator::getRenderState");
    }

    return itFind->second->getRenderState(materialName, groupName, passIndex);
}

//-----------------------------------------------------------------------------
void ShaderGenerator::addSceneManager(SceneManager* sceneMgr)
{
    // Make sure this scene manager not exists in the map.
    SceneManagerIterator itFind = mSceneManagerMap.find(sceneMgr->getName());
    
    if (itFind != mSceneManagerMap.end())
        return;

    if (!mRenderObjectListener)
        mRenderObjectListener.reset(new SGRenderObjectListener(this));
    
    sceneMgr->addRenderObjectListener(mRenderObjectListener.get());

    if (!mSceneManagerListener)
        mSceneManagerListener.reset(new SGSceneManagerListener(this));
    
    sceneMgr->addListener(mSceneManagerListener.get());

    mSceneManagerMap[sceneMgr->getName()] = sceneMgr;

    // Update the active scene manager.
    if (mActiveSceneMgr == NULL)
        mActiveSceneMgr = sceneMgr;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::removeSceneManager(SceneManager* sceneMgr)
{
    // Make sure this scene manager exists in the map.
    SceneManagerIterator itFind = mSceneManagerMap.find(sceneMgr->getName());
    
    if (itFind != mSceneManagerMap.end())
    {
        itFind->second->removeRenderObjectListener(mRenderObjectListener.get());
        itFind->second->removeListener(mSceneManagerListener.get());

        mSceneManagerMap.erase(itFind);

        // Update the active scene manager.
        if (mActiveSceneMgr == sceneMgr) {
            mActiveSceneMgr = NULL;

            // workaround for TextureUnit referencing a possibly deleted Frustum
            removeAllShaderBasedTechniques("Ogre/TextureShadowReceiver",
                    ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        }
    }
}

//-----------------------------------------------------------------------------
SceneManager* ShaderGenerator::getActiveSceneManager()
{
    return mActiveSceneMgr;
}
//-----------------------------------------------------------------------------
void ShaderGenerator::_setActiveSceneManager(SceneManager* sceneManager)
{
    mActiveViewportValid &= (mActiveSceneMgr == sceneManager);
    mActiveSceneMgr = sceneManager;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::setShaderProfiles(GpuProgramType type, const String& shaderProfiles)
{
    switch(type)
    {
    case GPT_VERTEX_PROGRAM:
        mVertexShaderProfiles = shaderProfiles;
        mVertexShaderProfilesList = StringUtil::split(shaderProfiles);
        break;
    case GPT_FRAGMENT_PROGRAM:
        mFragmentShaderProfiles = shaderProfiles;
        mFragmentShaderProfilesList = StringUtil::split(shaderProfiles);
        break;
    default:
        OgreAssert(false, "not implemented");
        break;
    }
}

const String& ShaderGenerator::getShaderProfiles(GpuProgramType type) const
{
    switch(type)
    {
    case GPT_VERTEX_PROGRAM:
        return mVertexShaderProfiles;
    case GPT_FRAGMENT_PROGRAM:
        return mFragmentShaderProfiles;
    default:
        return BLANKSTRING;
    }
}

const StringVector& ShaderGenerator::getShaderProfilesList(GpuProgramType type)
{
    switch(type)
    {
    case GPT_VERTEX_PROGRAM:
        return mVertexShaderProfilesList;
    case GPT_FRAGMENT_PROGRAM:
        return mFragmentShaderProfilesList;
    default:
        break;
    }

    static StringVector empty;
    return empty;
}
//-----------------------------------------------------------------------------
bool ShaderGenerator::hasShaderBasedTechnique(const String& materialName, 
                                              const String& srcTechniqueSchemeName, 
                                              const String& dstTechniqueSchemeName) const
{
    return hasShaderBasedTechnique(materialName, ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME,
        srcTechniqueSchemeName, dstTechniqueSchemeName);
}
//-----------------------------------------------------------------------------
bool ShaderGenerator::hasShaderBasedTechnique(const String& materialName, 
                                                 const String& groupName, 
                                                 const String& srcTechniqueSchemeName, 
                                                 const String& dstTechniqueSchemeName) const
{
    OGRE_LOCK_AUTO_MUTEX;

    // Make sure material exists;
    if (false == MaterialManager::getSingleton().resourceExists(materialName, groupName))
        return false;

    
    SGMaterialConstIterator itMatEntry = findMaterialEntryIt(materialName, groupName);
    
    // Check if technique already created.
    if (itMatEntry != mMaterialEntriesMap.end())
    {
        const SGTechniqueList& techniqueEntires = itMatEntry->second->getTechniqueList();
        SGTechniqueConstIterator itTechEntry = techniqueEntires.begin();

        for (; itTechEntry != techniqueEntires.end(); ++itTechEntry)
        {
            // Check requested mapping already exists.
            if ((*itTechEntry)->getSourceTechnique()->getSchemeName() == srcTechniqueSchemeName &&
                (*itTechEntry)->getDestinationTechniqueSchemeName() == dstTechniqueSchemeName)
            {
                return true;
            }           
        }
    }
    return false;
}
//-----------------------------------------------------------------------------
static bool hasFixedFunctionPass(Technique* tech)
{
    for (unsigned short i=0; i < tech->getNumPasses(); ++i)
    {
        if (!tech->getPass(i)->isProgrammable())
        {
            return true;
        }
    }
    return false;
}

static Technique* findSourceTechnique(const Material& mat, const String& srcTechniqueSchemeName,
                                      bool overProgrammable)
{
    // Find the source technique
    Material::Techniques::const_iterator it;
    for (it = mat.getTechniques().begin(); it != mat.getTechniques().end(); ++it)
    {
        Technique* curTechnique = *it;

        if (curTechnique->getSchemeName() == srcTechniqueSchemeName &&
            (hasFixedFunctionPass(curTechnique) || overProgrammable))
        {
            return curTechnique;
        }
    }

    return NULL;
}
//-----------------------------------------------------------------------------
bool ShaderGenerator::createShaderBasedTechnique(const Material& srcMat,
                                                 const String& srcTechniqueSchemeName, 
                                                 const String& dstTechniqueSchemeName,
                                                 bool overProgrammable)
{
    // No technique created -> check if one can be created from the given source technique scheme.  
    Technique* srcTechnique = findSourceTechnique(srcMat, srcTechniqueSchemeName, overProgrammable);

    // No appropriate source technique found.
    if (!srcTechnique)
    {
        return false;
    }

    return createShaderBasedTechnique(srcTechnique, dstTechniqueSchemeName, overProgrammable);
}
bool ShaderGenerator::createShaderBasedTechnique(const Technique* srcTechnique, const String& dstTechniqueSchemeName,
                                                 bool overProgrammable)
{
    OGRE_LOCK_AUTO_MUTEX;

    // Update group name in case it is AUTODETECT_RESOURCE_GROUP_NAME
    Material* srcMat = srcTechnique->getParent();
    const String& materialName = srcMat->getName();
    const String& trueGroupName = srcMat->getGroup();

    SGMaterialIterator itMatEntry = findMaterialEntryIt(materialName, trueGroupName);

    // Check if technique already created.
    if (itMatEntry != mMaterialEntriesMap.end())
    {
        const SGTechniqueList& techniqueEntires = itMatEntry->second->getTechniqueList();
        SGTechniqueConstIterator itTechEntry = techniqueEntires.begin();

        for (; itTechEntry != techniqueEntires.end(); ++itTechEntry)
        {
            // Case the requested mapping already exists.
            if ((*itTechEntry)->getSourceTechnique()->getSchemeName() == srcTechnique->getSchemeName() &&
                (*itTechEntry)->getDestinationTechniqueSchemeName() == dstTechniqueSchemeName)
            {
                return true;
            }


            // Case a shader based technique with the same scheme name already defined based
            // on different source technique.
            // This state might lead to conflicts during shader generation - we prevent it by returning false here.
            else if ((*itTechEntry)->getDestinationTechniqueSchemeName() == dstTechniqueSchemeName)
            {
                return false;
            }
        }
    }

    // Create shader based technique from the given source technique.
    SGMaterial* matEntry = NULL;

    if (itMatEntry == mMaterialEntriesMap.end())
    {
        matEntry = OGRE_NEW SGMaterial(materialName, trueGroupName);
        mMaterialEntriesMap.insert(SGMaterialMap::value_type(
            MatGroupPair(materialName, trueGroupName), matEntry));
    }
    else
    {
        matEntry = itMatEntry->second;
    }

    // Create the new technique entry.
    SGTechnique* techEntry =
        OGRE_NEW SGTechnique(matEntry, srcTechnique, dstTechniqueSchemeName, overProgrammable);

    // Add to material entry map.
    matEntry->getTechniqueList().push_back(techEntry);

    // Add to all technique map.
    mTechniqueEntriesMap[techEntry] = techEntry;

    // Add to scheme.
    SGScheme* schemeEntry = createOrRetrieveScheme(dstTechniqueSchemeName).first;
    schemeEntry->addTechniqueEntry(techEntry);

    return true;
}

bool ShaderGenerator::removeShaderBasedTechnique(const Technique* srcTech, const String& dstTechniqueSchemeName)
{
    OGRE_LOCK_AUTO_MUTEX;

    // Make sure scheme exists.
    SGSchemeIterator itScheme = mSchemeEntriesMap.find(dstTechniqueSchemeName);
    SGScheme* schemeEntry = NULL;

    if (itScheme == mSchemeEntriesMap.end())
        return false;

    schemeEntry = itScheme->second;

    // Find the material entry.
    Material* srcMat = srcTech->getParent();
    SGMaterialIterator itMatEntry = findMaterialEntryIt(srcMat->getName(), srcMat->getGroup());

    // Case material not found.
    if (itMatEntry == mMaterialEntriesMap.end())
        return false;


    SGTechniqueList& matTechniqueEntires = itMatEntry->second->getTechniqueList();
    SGTechniqueIterator itTechEntry = matTechniqueEntires.begin();
    SGTechnique* dstTechnique = NULL;

    // Remove destination technique entry from material techniques list.
    for (; itTechEntry != matTechniqueEntires.end(); ++itTechEntry)
    {
        if ((*itTechEntry)->getSourceTechnique()->getSchemeName() == srcTech->getSchemeName() &&
            (*itTechEntry)->getDestinationTechniqueSchemeName() == dstTechniqueSchemeName)
        {
            dstTechnique = *itTechEntry;
            matTechniqueEntires.erase(itTechEntry);
            break;
        }
    }

    // Technique not found.
    if (dstTechnique == NULL)
        return false;

    schemeEntry->removeTechniqueEntry(dstTechnique);

    SGTechniqueMapIterator itTechMap = mTechniqueEntriesMap.find(dstTechnique);

    if (itTechMap != mTechniqueEntriesMap.end())
        mTechniqueEntriesMap.erase(itTechMap);

    OGRE_DELETE dstTechnique;

    return true;
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::removeAllShaderBasedTechniques(const String& materialName, const String& groupName)
{
    OGRE_LOCK_AUTO_MUTEX;

    // Find the material entry.
    SGMaterialIterator itMatEntry = findMaterialEntryIt(materialName, groupName);
    
    // Case material not found.
    if (itMatEntry == mMaterialEntriesMap.end())
        return false;


    SGTechniqueList& matTechniqueEntires = itMatEntry->second->getTechniqueList();
        
    // Remove all technique entries from material techniques list.
    while (matTechniqueEntires.empty() == false)
    {   
        SGTechniqueIterator itTechEntry = matTechniqueEntires.begin();

        removeShaderBasedTechnique((*itTechEntry)->getSourceTechnique(),
                                   (*itTechEntry)->getDestinationTechniqueSchemeName());
    }

    OGRE_DELETE itMatEntry->second;
    mMaterialEntriesMap.erase(itMatEntry);
    
    return true;
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::cloneShaderBasedTechniques(const String& srcMaterialName, 
                                                 const String& srcGroupName, 
                                                 const String& dstMaterialName, 
                                                 const String& dstGroupName)
{
    OGRE_LOCK_AUTO_MUTEX;

    //
    // Check that both source and destination material exist
    //

    // Make sure material exists.
    MaterialPtr srcMat = MaterialManager::getSingleton().getByName(srcMaterialName, srcGroupName);
    MaterialPtr dstMat = MaterialManager::getSingleton().getByName(dstMaterialName, dstGroupName);
    if (!srcMat || !dstMat || (srcMat == dstMat))
        return false;

    // Update group name in case it is AUTODETECT_RESOURCE_GROUP_NAME
    const String& trueSrcGroupName = srcMat->getGroup();
    const String& trueDstGroupName = dstMat->getGroup();

    // Case the requested material belongs to different group and it is not AUTODETECT_RESOURCE_GROUP_NAME.
    if ((trueSrcGroupName != srcGroupName && srcGroupName != ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME) ||
        (trueSrcGroupName != dstGroupName && dstGroupName != ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME))
    {
        return false;
    }

    SGMaterialIterator itSrcMatEntry = findMaterialEntryIt(srcMaterialName, trueSrcGroupName);

    //remove any techniques in the destination material so the new techniques may be copied
    removeAllShaderBasedTechniques(dstMaterialName, trueDstGroupName);
    
    //
    //remove any techniques from the destination material which have RTSS associated schemes from
    //the source material. This code is performed in case the user performed a clone of a material
    //which has already generated RTSS techniques in the source material.
    //

    //first gather the techniques to remove
    std::set<unsigned short> schemesToRemove;
    unsigned short techCount = srcMat->getNumTechniques();
    for(unsigned short ti = 0 ; ti < techCount ; ++ti)
    {
        Technique* pSrcTech = srcMat->getTechnique(ti);
        Pass* pSrcPass = pSrcTech->getNumPasses() > 0 ? pSrcTech->getPass(0) : NULL;
        if (pSrcPass)
        {
            const Any& passUserData = pSrcPass->getUserObjectBindings().getUserAny(TargetRenderState::UserKey);
            if (passUserData.has_value())
            {
                schemesToRemove.insert(pSrcTech->_getSchemeIndex());
            }
        }
    }
    //remove the techniques from the destination material
    techCount = dstMat->getNumTechniques();
    for(unsigned short ti = techCount - 1 ; ti != (unsigned short)-1 ; --ti)
    {
        Technique* pDstTech = dstMat->getTechnique(ti);
        if (schemesToRemove.find(pDstTech->_getSchemeIndex()) != schemesToRemove.end())
        {
            dstMat->removeTechnique(ti);
        }
    }
    
    //
    // Clone the render states from source to destination
    //

    // Check if RTSS techniques exist in the source material 
    if (itSrcMatEntry != mMaterialEntriesMap.end())
    {
        const SGTechniqueList& techniqueEntires = itSrcMatEntry->second->getTechniqueList();
        SGTechniqueConstIterator itTechEntry = techniqueEntires.begin();

        //Go over all rtss techniques in the source material
        for (; itTechEntry != techniqueEntires.end(); ++itTechEntry)
        {
            String srcFromTechniqueScheme = (*itTechEntry)->getSourceTechnique()->getSchemeName();
            String srcToTechniqueScheme = (*itTechEntry)->getDestinationTechniqueSchemeName();
            
            //for every technique in the source material create a shader based technique in the 
            //destination material
            if (createShaderBasedTechnique(*dstMat, srcFromTechniqueScheme, srcToTechniqueScheme))
            {
                //check for custom render states in the source material
                unsigned short passCount =  (*itTechEntry)->getSourceTechnique()->getNumPasses();
                for(unsigned short pi = 0 ; pi < passCount ; ++pi)
                {
                    if ((*itTechEntry)->hasRenderState(pi))
                    {
                        //copy the custom render state from the source material to the destination material
                        RenderState* srcRenderState = (*itTechEntry)->getRenderState(pi);
                        RenderState* dstRenderState = getRenderState(srcToTechniqueScheme, dstMaterialName, trueDstGroupName, pi);

                        const SubRenderStateList& srcSubRenderState = 
                            srcRenderState->getTemplateSubRenderStateList();

                        SubRenderStateList::const_iterator itSubState = srcSubRenderState.begin(),
                            itSubStateEnd = srcSubRenderState.end();
                        for(;itSubState != itSubStateEnd ; ++itSubState)
                        {
                            SubRenderState* srcSubState = *itSubState;
                            SubRenderState* dstSubState = createSubRenderState(srcSubState->getType());
                            (*dstSubState) = (*srcSubState);
                            dstRenderState->addTemplateSubRenderState(dstSubState);
                        }
                    }
                }
            }
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::removeAllShaderBasedTechniques()
{
    OGRE_LOCK_AUTO_MUTEX;

    while (!mMaterialEntriesMap.empty())
    {
        SGMaterialIterator itMatEntry = mMaterialEntriesMap.begin();

        removeAllShaderBasedTechniques(itMatEntry->first.first, itMatEntry->first.second);
    }
}

//-----------------------------------------------------------------------------
 void ShaderGenerator::notifyRenderSingleObject(Renderable* rend, 
     const Pass* pass,  
     const AutoParamDataSource* source, 
     const LightList* pLightList, bool suppressRenderStateChanges)
{
    if (mActiveViewportValid)
    {
        const Any& passUserData = pass->getUserObjectBindings().getUserAny(TargetRenderState::UserKey);

        if (!passUserData.has_value() || suppressRenderStateChanges)
            return; 

        OGRE_LOCK_AUTO_MUTEX;

        auto renderState = any_cast<TargetRenderState*>(passUserData);
        renderState->updateGpuProgramsParams(rend, pass, source, pLightList);        
    }   
}


//-----------------------------------------------------------------------------
void ShaderGenerator::preFindVisibleObjects(SceneManager* source, 
                                            SceneManager::IlluminationRenderStage irs, 
                                            Viewport* v)
{
    OGRE_LOCK_AUTO_MUTEX;

    const String& curMaterialScheme = v->getMaterialScheme();
        
    mActiveSceneMgr      = source;
    mActiveViewportValid = validateScheme(curMaterialScheme);
}

//-----------------------------------------------------------------------------
void ShaderGenerator::invalidateScheme(const String& schemeName)
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);

    if (itScheme != mSchemeEntriesMap.end())    
        itScheme->second->invalidate(); 
    
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::validateScheme(const String& schemeName)
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);

    // No such scheme exists.
    if (itScheme == mSchemeEntriesMap.end())    
        return false;

    itScheme->second->validate();

    return true;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::invalidateMaterial(const String& schemeName, const String& materialName, const String& groupName)
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);
    
    if (itScheme != mSchemeEntriesMap.end())            
        itScheme->second->invalidate(materialName, groupName);  
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::validateMaterial(const String& schemeName, const String& materialName, const String& groupName)
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);

    // No such scheme exists.
    if (itScheme == mSchemeEntriesMap.end())    
        return false;

    return itScheme->second->validate(materialName, groupName); 
}

//-----------------------------------------------------------------------------
void ShaderGenerator::invalidateMaterialIlluminationPasses(const String& schemeName, const String& materialName, const String& groupName)
{
	OGRE_LOCK_AUTO_MUTEX;

	SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);

	if(itScheme != mSchemeEntriesMap.end())
		itScheme->second->invalidateIlluminationPasses(materialName, groupName);
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::validateMaterialIlluminationPasses(const String& schemeName, const String& materialName, const String& groupName)
{
	OGRE_LOCK_AUTO_MUTEX;

	SGSchemeIterator itScheme = mSchemeEntriesMap.find(schemeName);

	// No such scheme exists.
	if(itScheme == mSchemeEntriesMap.end())
		return false;

	return itScheme->second->validateIlluminationPasses(materialName, groupName);
}

//-----------------------------------------------------------------------------
MaterialSerializer::Listener* ShaderGenerator::getMaterialSerializerListener()
{
    if (!mMaterialSerializerListener)
        mMaterialSerializerListener.reset(new SGMaterialSerializerListener);

    return mMaterialSerializerListener.get();
}

//-----------------------------------------------------------------------------
void ShaderGenerator::flushShaderCache()
{
    SGTechniqueMapIterator itTech = mTechniqueEntriesMap.begin();
    SGTechniqueMapIterator itTechEnd = mTechniqueEntriesMap.end();

    // Release all programs.
    for (; itTech != itTechEnd; ++itTech)
    {           
        itTech->second->releasePrograms();
    }

    ProgramManager::getSingleton().flushGpuProgramsCache();
    
    SGSchemeIterator itScheme = mSchemeEntriesMap.begin();
    SGSchemeIterator itSchemeEnd = mSchemeEntriesMap.end();

    // Invalidate all schemes.
    for (; itScheme != itSchemeEnd; ++itScheme)
    {
        itScheme->second->invalidate();
    }
}

//-----------------------------------------------------------------------------
ScriptTranslator* ShaderGenerator::getTranslator(const AbstractNodePtr& node)
{
    OGRE_LOCK_AUTO_MUTEX;

    if(node->type != ANT_OBJECT)
        return NULL;
    
    ObjectAbstractNode *obj = static_cast<ObjectAbstractNode*>(node.get());

    if(obj->id == ID_RT_SHADER_SYSTEM)
        return &mCoreScriptTranslator;
    
    return NULL;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::serializePassAttributes(MaterialSerializer* ser, SGPass* passEntry)
{
    
    // Write section header and begin it.
    ser->writeAttribute(3, "rtshader_system");
    ser->beginSection(3);

    // Grab the custom render state this pass uses.
    RenderState* customRenderState = passEntry->getCustomRenderState();

    if (customRenderState != NULL)
    {
        // Write each of the sub-render states that composing the final render state.
        const SubRenderStateList& subRenderStates = customRenderState->getTemplateSubRenderStateList();
        SubRenderStateListConstIterator it      = subRenderStates.begin();
        SubRenderStateListConstIterator itEnd   = subRenderStates.end();

        for (; it != itEnd; ++it)
        {
            SubRenderState* curSubRenderState = *it;
            SubRenderStateFactoryIterator itFactory = mSubRenderStateFactories.find(curSubRenderState->getType());

            if (itFactory != mSubRenderStateFactories.end())
            {
                SubRenderStateFactory* curFactory = itFactory->second;
                curFactory->writeInstance(ser, curSubRenderState, passEntry->getSrcPass(), passEntry->getDstPass());
            }
        }
    }
    
    // Write section end.
    ser->endSection(3);     
}



//-----------------------------------------------------------------------------
void ShaderGenerator::serializeTextureUnitStateAttributes(MaterialSerializer* ser, SGPass* passEntry, const TextureUnitState* srcTextureUnit)
{
    
    // Write section header and begin it.
    ser->writeAttribute(4, "rtshader_system");
    ser->beginSection(4);

    // Grab the custom render state this pass uses.
    RenderState* customRenderState = passEntry->getCustomRenderState();
            
    if (customRenderState != NULL)
    {
        //retrive the destintion texture unit state
        TextureUnitState* dstTextureUnit = NULL;
        unsigned short texIndex = srcTextureUnit->getParent()->getTextureUnitStateIndex(srcTextureUnit);
        if (texIndex < passEntry->getDstPass()->getNumTextureUnitStates())
        {
            dstTextureUnit = passEntry->getDstPass()->getTextureUnitState(texIndex);
        }
        
        // Write each of the sub-render states that composing the final render state.
        const SubRenderStateList& subRenderStates = customRenderState->getTemplateSubRenderStateList();
        SubRenderStateListConstIterator it      = subRenderStates.begin();
        SubRenderStateListConstIterator itEnd   = subRenderStates.end();

        for (; it != itEnd; ++it)
        {
            SubRenderState* curSubRenderState = *it;
            SubRenderStateFactoryIterator itFactory = mSubRenderStateFactories.find(curSubRenderState->getType());

            if (itFactory != mSubRenderStateFactories.end())
            {
                SubRenderStateFactory* curFactory = itFactory->second;
                curFactory->writeInstance(ser, curSubRenderState, srcTextureUnit, dstTextureUnit);
            }
        }
    }
    
    // Write section end.
    ser->endSection(4);     
}

//-----------------------------------------------------------------------------
size_t ShaderGenerator::getShaderCount(GpuProgramType type) const
{
    return mProgramManager->getShaderCount(type);
}

//-----------------------------------------------------------------------------
void ShaderGenerator::setTargetLanguage(const String& shaderLanguage)
{
    // Make sure that the shader language is supported.
    if (!mProgramWriterManager->isLanguageSupported(shaderLanguage))
    {
        OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR, "'" + shaderLanguage + "' is not supported");
    }

    // Case target language changed -> flush the shaders cache.
    if (mShaderLanguage != shaderLanguage)
    {
        mShaderLanguage = shaderLanguage;
        flushShaderCache();
    }
}

//-----------------------------------------------------------------------------
void ShaderGenerator::setShaderCachePath( const String& cachePath )
{
    String stdCachePath = cachePath;

    // Standardise the cache path in case of none empty string.
    if (stdCachePath.empty() == false)
        stdCachePath = StringUtil::standardisePath(stdCachePath);

    if (mShaderCachePath != stdCachePath)
    {
        mShaderCachePath = stdCachePath;

        // Case this is a valid file path -> add as resource location in order to make sure that
        // generated shaders could be loaded by the file system archive.
        if (mShaderCachePath.empty() == false)
        {   
            // Make sure this is a valid writable path.
            String outTestFileName(mShaderCachePath + "ShaderGenerator.tst");
            std::ofstream outFile(outTestFileName.c_str());
            
            if (!outFile)
            {
                OGRE_EXCEPT(Exception::ERR_CANNOT_WRITE_TO_FILE,
                    "Could not create output files in the given shader cache path '" + mShaderCachePath,
                    "ShaderGenerator::setShaderCachePath"); 
            }

            // Close and remove the test file.
            outFile.close();
            remove(outTestFileName.c_str());
        }
    }
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGMaterialIterator ShaderGenerator::findMaterialEntryIt(const String& materialName, const String& groupName)
{
    SGMaterialIterator itMatEntry;
    //check if we have auto detect request
    if (groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME)
    {
        //find the possible first entry
        itMatEntry = mMaterialEntriesMap.lower_bound(MatGroupPair(materialName,""));
        if ((itMatEntry != mMaterialEntriesMap.end()) &&
            (itMatEntry->first.first != materialName))
        {
            //no entry found
            itMatEntry = mMaterialEntriesMap.end();
        }
    }
    else
    {
        //find entry with group name specified
        itMatEntry = mMaterialEntriesMap.find(MatGroupPair(materialName,groupName));
    }
    return itMatEntry;
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGMaterialConstIterator ShaderGenerator::findMaterialEntryIt(const String& materialName, const String& groupName) const
{
    SGMaterialConstIterator itMatEntry;
    //check if we have auto detect request
    if (groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME)
    {
        //find the possible first entry
        itMatEntry = mMaterialEntriesMap.lower_bound(MatGroupPair(materialName,""));
        if ((itMatEntry != mMaterialEntriesMap.end()) &&
            (itMatEntry->first.first != materialName))
        {
            //no entry found
            itMatEntry = mMaterialEntriesMap.end();
        }
    }
    else
    {
        //find entry with group name specified
        itMatEntry = mMaterialEntriesMap.find(MatGroupPair(materialName,groupName));
    }
    return itMatEntry;
}
//-----------------------------------------------------------------------------
size_t ShaderGenerator::getRTShaderSchemeCount() const
{
    OGRE_LOCK_AUTO_MUTEX;
    return mSchemeEntriesMap.size();
}
//-----------------------------------------------------------------------------
const String& ShaderGenerator::getRTShaderScheme(size_t index) const
{
    OGRE_LOCK_AUTO_MUTEX;

    SGSchemeMap::const_iterator it = mSchemeEntriesMap.begin();
    while ((index != 0) && (it != mSchemeEntriesMap.end()))
    {
        --index;
        ++it;
    }

    assert((it != mSchemeEntriesMap.end()) && "Index out of bounds");
    if (it != mSchemeEntriesMap.end())
        return it->first;
    else return BLANKSTRING;
}

//-----------------------------------------------------------------------------

bool ShaderGenerator::getIsFinalizing() const
{
    return mIsFinalizing;
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGPassList ShaderGenerator::createSGPassList(Material* mat) const
{
    SGPassList passList;

    SGMaterialConstIterator itMatEntry = findMaterialEntryIt(mat->getName(), mat->getGroup());

    // Check if material is managed.
    if (itMatEntry == mMaterialEntriesMap.end())
        return passList;

    for (auto sgtech : itMatEntry->second->getTechniqueList())
    {
        for(auto sgpass : sgtech->getPassList())
        {
            passList.push_back(sgpass);
        }
    }

    return passList;
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGPass::SGPass(SGTechnique* parent, Pass* srcPass, Pass* dstPass, IlluminationStage stage)
{
    mParent             = parent;
    mSrcPass            = srcPass;
	mDstPass			= dstPass;
	mStage				= stage;
    mCustomRenderState  = NULL;
    mTargetRenderState  = NULL;
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGPass::~SGPass()
{
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGPass::buildTargetRenderState()
{   
    if(mSrcPass->isProgrammable() && !mParent->overProgrammablePass() && !isIlluminationPass()) return;
    const String& schemeName = mParent->getDestinationTechniqueSchemeName();
    const RenderState* renderStateGlobal = ShaderGenerator::getSingleton().getRenderState(schemeName);
    

    mTargetRenderState.reset(new TargetRenderState);

    // Set light properties.
    Vector3i lightCount(0, 0, 0);

    // Use light count definitions of the custom render state if exists.
    if (mCustomRenderState != NULL && mCustomRenderState->getLightCountAutoUpdate() == false)
    {
        lightCount = mCustomRenderState->getLightCount();
    }

    // Use light count definitions of the global render state if exists.
    else if (renderStateGlobal != NULL)
    {
        lightCount = renderStateGlobal->getLightCount();
    }
    
    
    mTargetRenderState->setLightCount(lightCount);
            
#ifdef RTSHADER_SYSTEM_BUILD_CORE_SHADERS
    // Build the FFP state. 
    FFPRenderStateBuilder::buildRenderState(this, mTargetRenderState.get());
#endif


    // Link the target render state with the custom render state of this pass if exists.
    if (mCustomRenderState != NULL)
    {
        mTargetRenderState->link(*mCustomRenderState, mSrcPass, mDstPass);
    }

    // Link the target render state with the scheme render state of the shader generator.
    if (renderStateGlobal != NULL)
    {
        mTargetRenderState->link(*renderStateGlobal, mSrcPass, mDstPass);
    }               
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGPass::acquirePrograms()
{
    if(!mTargetRenderState) return;
    mTargetRenderState->acquirePrograms(mDstPass);
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGPass::releasePrograms()
{
    if(!mTargetRenderState) return;
    mTargetRenderState->releasePrograms(mDstPass);
}
//-----------------------------------------------------------------------------
ShaderGenerator::SGTechnique::SGTechnique(SGMaterial* parent, const Technique* srcTechnique,
                                          const String& dstTechniqueSchemeName,
                                          bool overProgrammable)
    : mParent(parent), mSrcTechnique(srcTechnique), mDstTechnique(NULL), mBuildDstTechnique(true),
      mDstTechniqueSchemeName(dstTechniqueSchemeName), mOverProgrammable(overProgrammable)
{
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::createSGPasses()
{
    // Create pass entry for each pass.
    for (unsigned short i=0; i < mSrcTechnique->getNumPasses(); ++i)
    {
        Pass* srcPass = mSrcTechnique->getPass(i);
        Pass* dstPass = mDstTechnique->getPass(i);

		SGPass* passEntry = OGRE_NEW SGPass(this, srcPass, dstPass, IS_UNKNOWN);

        if (i < mCustomRenderStates.size())
            passEntry->setCustomRenderState(mCustomRenderStates[i]);
        mPassEntries.push_back(passEntry);
    }
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::createIlluminationSGPasses()
{
	// Create pass entry for each illumination pass.
	const IlluminationPassList& passes = mDstTechnique->getIlluminationPasses();

	for(size_t i = 0; i < passes.size(); i++)
	{
		// process only autogenerated illumination passes
		IlluminationPass* p = passes[i];
		if(p->pass == p->originalPass)
			continue;

		SGPass* passEntry = OGRE_NEW SGPass(this, p->pass, p->pass, p->stage);

		const Any& origPassUserData = p->originalPass->getUserObjectBindings().getUserAny(TargetRenderState::UserKey);
		if(origPassUserData.has_value())
		{
            for(auto sgp : mPassEntries)
            {
                if(sgp->getDstPass() == p->originalPass)
                {
                    passEntry->setCustomRenderState(sgp->getCustomRenderState());
                    break;
                }
            }
		}

		mPassEntries.push_back(passEntry);
	}
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::destroyIlluminationSGPasses()
{
	for(SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); /* no increment*/ )
	{
		if((*itPass)->isIlluminationPass())
		{
			OGRE_DELETE(*itPass);
			itPass = mPassEntries.erase(itPass);
		}
		else
		{
			++itPass;
		}
	}
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGTechnique::~SGTechnique()
{
    const String& materialName = mParent->getMaterialName();
    const String& groupName = mParent->getGroupName();

    // Release CPU/GPU programs that associated with this technique passes.
    // Needs the parent technique to still exist
    for (SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
    {
        (*itPass)->releasePrograms();
    }

    if (MaterialManager::getSingleton().resourceExists(materialName, groupName))
    {
        MaterialPtr mat = MaterialManager::getSingleton().getByName(materialName, groupName);
    
        // Remove the destination technique from parent material.
        for (ushort i=0; i < mat->getNumTechniques(); ++i)
        {
            if (mDstTechnique == mat->getTechnique(i))
            {
                // Unload the generated technique in order tor free referenced resources.
                mDstTechnique->_unload();

                // Remove the generated technique in order to restore the material to its original state.
                mat->removeTechnique(i);

                // touch when finalizing - will reload the textures - so no touch if finalizing
                if (ShaderGenerator::getSingleton().getIsFinalizing() == false)
                {
                    // Make sure the material goes back to its original state.
                    mat->touch();
                }
                break;
            }       
        }
    }
    
    // Destroy the passes.
    destroySGPasses();

    // Delete the custom render states of each pass if exist.
    for (unsigned int i=0; i < mCustomRenderStates.size(); ++i)
    {
        if (mCustomRenderStates[i] != NULL)
        {
            OGRE_DELETE mCustomRenderStates[i];
            mCustomRenderStates[i] = NULL;
        }       
    }
    mCustomRenderStates.clear();
    
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::destroySGPasses()
{
    for (SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
    {
        OGRE_DELETE (*itPass);
    }
    mPassEntries.clear();
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::buildTargetRenderState()
{
    // Remove existing destination technique and passes
    // in order to build it again from scratch.
    if (mDstTechnique != NULL)
    {
        Material* mat = mSrcTechnique->getParent();

        for (unsigned short i=0; i < mat->getNumTechniques(); ++i)
        {
            if (mat->getTechnique(i) == mDstTechnique)
            {
                mat->removeTechnique(i);
                break;
            }
        }
        destroySGPasses();      
    }
        
    // Create the destination technique and passes.
    mDstTechnique   = mSrcTechnique->getParent()->createTechnique();
    mDstTechnique->getUserObjectBindings().setUserAny(SGTechnique::UserKey, Any(this));
    *mDstTechnique  = *mSrcTechnique;
    mDstTechnique->setSchemeName(mDstTechniqueSchemeName);
    createSGPasses();


    // Build render state for each pass.
    for (SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
    {
		assert(!(*itPass)->isIlluminationPass()); // this is not so important, but intended to be so here.
        (*itPass)->buildTargetRenderState();
    }
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::acquirePrograms()
{
	for(SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
		if(!(*itPass)->isIlluminationPass())
			(*itPass)->acquirePrograms();
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::buildIlluminationTargetRenderState()
{
	assert(mDstTechnique != NULL);
	assert(!getBuildDestinationTechnique());

	// Create the illumination passes.
	createIlluminationSGPasses();

	// Build render state for each pass.
	for(SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
	{
		if((*itPass)->isIlluminationPass())
			(*itPass)->buildTargetRenderState();
	}
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::acquireIlluminationPrograms()
{
	for(SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
		if((*itPass)->isIlluminationPass())
			(*itPass)->acquirePrograms();
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGTechnique::releasePrograms()
{
    // Remove destination technique.
    if (mDstTechnique != NULL)
    {
        Material* mat = mSrcTechnique->getParent();

        for (unsigned short i=0; i < mat->getNumTechniques(); ++i)
        {
            if (mat->getTechnique(i) == mDstTechnique)
            {
                mat->removeTechnique(i);
                break;
            }
        }
        mDstTechnique = NULL;
    }

    // Release CPU/GPU programs that associated with this technique passes.
    for (SGPassIterator itPass = mPassEntries.begin(); itPass != mPassEntries.end(); ++itPass)
    {
        (*itPass)->releasePrograms();
    }

    // Destroy the passes.
    destroySGPasses();
}

//-----------------------------------------------------------------------------
RenderState* ShaderGenerator::SGTechnique::getRenderState(unsigned short passIndex)
{
    RenderState* renderState = NULL;

    if (passIndex >= mCustomRenderStates.size())    
        mCustomRenderStates.resize(passIndex + 1, NULL);        

    renderState = mCustomRenderStates[passIndex];
    if (renderState == NULL)
    {
        renderState = OGRE_NEW RenderState;
        mCustomRenderStates[passIndex] = renderState;
    }
    
    return renderState;
}
//-----------------------------------------------------------------------------
bool ShaderGenerator::SGTechnique::hasRenderState(unsigned short passIndex)
{
    return (passIndex < mCustomRenderStates.size()) && (mCustomRenderStates[passIndex] != NULL);
}


//-----------------------------------------------------------------------------
ShaderGenerator::SGScheme::SGScheme(const String& schemeName) :
    mName(schemeName), mOutOfDate(true), mFogMode(FOG_NONE)
{
}

//-----------------------------------------------------------------------------
ShaderGenerator::SGScheme::~SGScheme()
{
}

//-----------------------------------------------------------------------------
RenderState* ShaderGenerator::SGScheme::getRenderState()
{
    if (!mRenderState)
        mRenderState.reset(new RenderState);

    return mRenderState.get();
}

//-----------------------------------------------------------------------------
RenderState* ShaderGenerator::SGScheme::getRenderState(const String& materialName, const String& groupName, unsigned short passIndex)
{
    SGTechniqueIterator itTech;

    // Find the desired technique.
    bool doAutoDetect = groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME;
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;
        Material* curMat = curTechEntry->getSourceTechnique()->getParent();
        if ((curMat->getName() == materialName) && 
            ((doAutoDetect == true) || (curMat->getGroup() == groupName)))
        {
            return curTechEntry->getRenderState(passIndex);         
        }
    }

    return NULL;
}


//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::addTechniqueEntry(SGTechnique* techEntry)
{
    mTechniqueEntries.push_back(techEntry);

    // Mark as out of data.
    mOutOfDate = true;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::removeTechniqueEntry(SGTechnique* techEntry)
{
    SGTechniqueIterator itTech;

    // Build render state for each technique.
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;

        if (curTechEntry == techEntry)
        {
            mTechniqueEntries.erase(itTech);
            break;
        }       
    }
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::validate()
{   
    // Synchronize with light settings.
    synchronizeWithLightSettings();

    // Synchronize with fog settings.
    synchronizeWithFogSettings();

    // The target scheme is up to date.
    if (mOutOfDate == false)
        return;
    
    SGTechniqueIterator itTech;

    // Build render state for each technique.
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;

        if (curTechEntry->getBuildDestinationTechnique())
            curTechEntry->buildTargetRenderState();     
    }

    // Acquire GPU programs for each technique.
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;
        
        if (curTechEntry->getBuildDestinationTechnique())
            curTechEntry->acquirePrograms();        
    }

    // Turn off the build destination technique flag.
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;

        curTechEntry->setBuildDestinationTechnique(false);
    }
    
    // Mark this scheme as up to date.
    mOutOfDate = false;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::synchronizeWithLightSettings()
{
    SceneManager* sceneManager = ShaderGenerator::getSingleton().getActiveSceneManager();
    RenderState* curRenderState = getRenderState();

    if (sceneManager != NULL && curRenderState->getLightCountAutoUpdate())
    {
        const LightList& lightList =  sceneManager->_getLightsAffectingFrustum();
        
        Vector3i sceneLightCount(0, 0, 0);
        for (unsigned int i=0; i < lightList.size(); ++i)
        {
            sceneLightCount[lightList[i]->getType()]++;
        }
        
        auto currLightCount = mRenderState->getLightCount();

        // Case light state has been changed -> invalidate this scheme.
        if (currLightCount != sceneLightCount)
        {       
            curRenderState->setLightCount(sceneLightCount);
            invalidate();
        }
    }
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::synchronizeWithFogSettings()
{
    SceneManager* sceneManager = ShaderGenerator::getSingleton().getActiveSceneManager();

    if (sceneManager != NULL && sceneManager->getFogMode() != mFogMode)
    {
        mFogMode = sceneManager->getFogMode();
        invalidate();
    }
}

//-----------------------------------------------------------------------------
bool ShaderGenerator::SGScheme::validate(const String& materialName, const String& groupName)
{
    // Synchronize with light settings.
    synchronizeWithLightSettings();

    // Synchronize with fog settings.
    synchronizeWithFogSettings();


    SGTechniqueIterator itTech;
    
    // Find the desired technique.
    bool doAutoDetect = groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME;
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;
        const SGMaterial* curMat = curTechEntry->getParent();
        if ((curMat->getMaterialName() == materialName) && 
            ((doAutoDetect == true) || (curMat->getGroupName() == groupName)) &&
            (curTechEntry->getBuildDestinationTechnique()))
        {       
            // Build render state for each technique.
            curTechEntry->buildTargetRenderState();

            // Acquire the CPU/GPU programs.
            curTechEntry->acquirePrograms();

            // Turn off the build destination technique flag.
            curTechEntry->setBuildDestinationTechnique(false);

			return true;
        }                   
    }

    return false;
}
//-----------------------------------------------------------------------------
bool ShaderGenerator::SGScheme::validateIlluminationPasses(const String& materialName, const String& groupName)
{
	// Synchronize with light settings.
	synchronizeWithLightSettings();

	// Synchronize with fog settings.
	synchronizeWithFogSettings();


	SGTechniqueIterator itTech;

	// Find the desired technique.
	bool doAutoDetect = groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME;
	for(itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
	{
		SGTechnique* curTechEntry = *itTech;
		const SGMaterial* curMat = curTechEntry->getParent();
		if((curMat->getMaterialName() == materialName) &&
			((doAutoDetect == true) || (curMat->getGroupName() == groupName)))
		{
			// Build render state for each technique.
			curTechEntry->buildIlluminationTargetRenderState();

			// Acquire the CPU/GPU programs.
			curTechEntry->acquireIlluminationPrograms();

			return true;
		}
	}

	return false;
}
//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::invalidateIlluminationPasses(const String& materialName, const String& groupName)
{
	SGTechniqueIterator itTech;

	// Find the desired technique.
	bool doAutoDetect = groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME;
	for(itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
	{
		SGTechnique* curTechEntry = *itTech;
		const SGMaterial* curMat = curTechEntry->getParent();
		if((curMat->getMaterialName() == materialName) &&
			((doAutoDetect == true) || (curMat->getGroupName() == groupName)))
		{
			curTechEntry->destroyIlluminationSGPasses();
		}
	}
}
//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::invalidate(const String& materialName, const String& groupName)
{
    SGTechniqueIterator itTech;

    // Find the desired technique.
    bool doAutoDetect = groupName == ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME;
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;
        const SGMaterial* curMaterial = curTechEntry->getParent();
        if ((curMaterial->getMaterialName() == materialName) &&
            ((doAutoDetect == true) || (curMaterial->getGroupName() == groupName))) 
        {           
            // Turn on the build destination technique flag.
            curTechEntry->setBuildDestinationTechnique(true);
            break;          
        }                   
    }

    mOutOfDate = true;
}

//-----------------------------------------------------------------------------
void ShaderGenerator::SGScheme::invalidate()
{   
    SGTechniqueIterator itTech;
    
    // Turn on the build destination technique flag of all techniques.
    for (itTech = mTechniqueEntries.begin(); itTech != mTechniqueEntries.end(); ++itTech)
    {
        SGTechnique* curTechEntry = *itTech;

        curTechEntry->setBuildDestinationTechnique(true);
    }

    mOutOfDate = true;          
}

}
}