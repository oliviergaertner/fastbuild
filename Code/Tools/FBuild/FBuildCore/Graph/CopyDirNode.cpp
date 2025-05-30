// CopyDirNode.cpp
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "CopyDirNode.h"

#include "Tools/FBuild/FBuildCore/BFF/Functions/Function.h"
#include "Tools/FBuild/FBuildCore/FBuild.h"
#include "Tools/FBuild/FBuildCore/FLog.h"
#include "Tools/FBuild/FBuildCore/Graph/CopyFileNode.h"
#include "Tools/FBuild/FBuildCore/Graph/DirectoryListNode.h"
#include "Tools/FBuild/FBuildCore/Graph/NodeGraph.h"

#include "Core/Math/xxHash.h"
#include "Core/Strings/AStackString.h"

// REFLECTION
//------------------------------------------------------------------------------
REFLECT_NODE_BEGIN( CopyDirNode, Node, MetaNone() )
    REFLECT_ARRAY(  m_SourcePaths,              "SourcePaths",              MetaPath() )
    REFLECT(        m_Dest,                     "Dest",                     MetaPath() )
    REFLECT_ARRAY(  m_SourcePathsPattern,       "SourcePathsPattern",       MetaOptional() )
    REFLECT_ARRAY(  m_SourceExcludePaths,       "SourceExcludePaths",       MetaOptional() + MetaPath() )
    REFLECT(        m_SourcePathsRecurse,       "SourcePathsRecurse",       MetaOptional() )
    REFLECT_ARRAY(  m_PreBuildDependencyNames,  "PreBuildDependencies",     MetaOptional() + MetaFile() + MetaAllowNonFile() )
REFLECT_END( CopyDirNode )

// CONSTRUCTOR
//------------------------------------------------------------------------------
CopyDirNode::CopyDirNode()
    : Node( Node::COPY_DIR_NODE )
{
}

// Initialize
//------------------------------------------------------------------------------
/*virtual*/ bool CopyDirNode::Initialize( NodeGraph & nodeGraph, const BFFToken * iter, const Function * function )
{
    // .PreBuildDependencies
    if ( !InitializePreBuildDependencies( nodeGraph, iter, function, m_PreBuildDependencyNames ) )
    {
        return false; // InitializePreBuildDependencies will have emitted an error
    }

    // .CompilerInputPath
    Dependencies sourcePaths;
    if ( !Function::GetDirectoryListNodeList( nodeGraph,
                                              iter,
                                              function,
                                              m_SourcePaths,
                                              m_SourceExcludePaths,
                                              Array< AString >(),     // Unsupported: Excluded files
                                              Array< AString >(),    // Unsupported: Excluded patterns
                                              m_SourcePathsRecurse,
                                              false, // Don't include read-only status in hash
                                              false, // Don't include directories
                                              &m_SourcePathsPattern,
                                              "SourcePaths",
                                              sourcePaths ) )
    {
        return false; // GetDirectoryListNodeList will have emitted an error
    }
    ASSERT( sourcePaths.GetSize() == m_SourcePaths.GetSize() );

    // Store dependencies
    m_StaticDependencies.SetCapacity( sourcePaths.GetSize() );
    m_StaticDependencies.Add( sourcePaths );

    return true;
}

// DESTRUCTOR
//------------------------------------------------------------------------------
CopyDirNode::~CopyDirNode() = default;

// IsAFile
//------------------------------------------------------------------------------
/*virtual*/ bool CopyDirNode::IsAFile() const
{
    return false;
}

// DoDynamicDependencies
//------------------------------------------------------------------------------
/*virtual*/ bool CopyDirNode::DoDynamicDependencies( NodeGraph & nodeGraph )
{
    m_DynamicDependencies.Clear();

    ASSERT( !m_StaticDependencies.IsEmpty() );

    Array< AString > preBuildDependencyNames;
    preBuildDependencyNames.SetCapacity( m_PreBuildDependencies.GetSize() );
    for ( const Dependency & dep : m_PreBuildDependencies )
    {
        preBuildDependencyNames.Append( dep.GetNode()->GetName() );
    }

    // Iterate all the DirectoryListNodes
    for ( const Dependency & dep : m_StaticDependencies )
    {
        // Grab the files
        const DirectoryListNode * dln = dep.GetNode()->CastTo< DirectoryListNode >();
        for ( const FileIO::FileInfo & file : dln->GetFiles() )
        {
            // Create a CopyFileNode for each dynamically discovered file

            // source file (full path)
            const AString & srcFile = file.m_Name;

            // source file (relative to base path)
            const AStackString<> srcFileRel( srcFile.Get() + dln->GetPath().GetLength() );

            // source file (as a node)
            Node * srcFileNode = nodeGraph.FindNode( srcFile );
            if ( srcFileNode == nullptr )
            {
                srcFileNode = nodeGraph.CreateNode<FileNode>( srcFile );
            }
            else if ( srcFileNode->IsAFile() == false )
            {
                FLOG_ERROR( "CopyDir() Node '%s' is not a FileNode (type: %s)", srcFile.Get(), srcFileNode->GetTypeName() );
                return false;
            }

            // generate dest file name
            AStackString<> dstFile( m_Dest );
            dstFile += srcFileRel;

            // make sure dest doesn't already exist
            Node * n = nodeGraph.FindNode( dstFile );
            if ( n == nullptr )
            {
                CopyFileNode * copyFileNode = nodeGraph.CreateNode<CopyFileNode>( dstFile );
                copyFileNode->m_Source = srcFileNode->GetName();
                copyFileNode->m_PreBuildDependencyNames = preBuildDependencyNames; // inherit PreBuildDependencies
                const BFFToken * token = nullptr;
                if ( !copyFileNode->Initialize( nodeGraph, token, nullptr ) )
                {
                    return false; // Initialize will have emitted an error
                }
                n = copyFileNode;
            }
            else if ( n->GetType() != Node::COPY_FILE_NODE )
            {
                FLOG_ERROR( "Node '%s' is not a CopyFileNode (type: %s)", n->GetName().Get(), n->GetTypeName() );
                return false;
            }
            else
            {
                const CopyFileNode * cn = n->CastTo< CopyFileNode >();
                if ( srcFileNode != cn->GetSourceNode() )
                {
                    FLOG_ERROR( "Conflicting objects found during CopyDir:\n"
                                " File A: %s\n"
                                " File B: %s\n"
                                " Both copy to: %s\n",
                                srcFile.Get(),
                                cn->GetSourceNode()->GetName().Get(),
                                dstFile.Get() );
                    return false;
                }
            }

            m_DynamicDependencies.Add( n );
        }
    }
    return true;
}

// DoBuild
//------------------------------------------------------------------------------
/*virtual*/ Node::BuildResult CopyDirNode::DoBuild( Job * /*job*/ )
{
    if (m_DynamicDependencies.IsEmpty())
    {
        m_Stamp = 1; // Non-zero
    }
    else
    {
        // Generate stamp
        StackArray< uint64_t > stamps;
        stamps.SetCapacity( m_DynamicDependencies.GetSize() );
        for ( const Dependency & dep: m_DynamicDependencies )
        {
            const CopyFileNode * cn = dep.GetNode()->CastTo< CopyFileNode >();
            ASSERT( cn->GetStamp() );
            stamps.Append( cn->GetStamp() );
        }
        m_Stamp = xxHash3::Calc64( &stamps[ 0 ], ( stamps.GetSize() * sizeof( uint64_t ) ) );
    }

    return BuildResult::eOk;
}

//------------------------------------------------------------------------------
