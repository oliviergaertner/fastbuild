// FunctionAlias
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "FunctionCopy.h"
#include "Tools/FBuild/FBuildCore/FBuild.h"
#include "Tools/FBuild/FBuildCore/FLog.h"
#include "Tools/FBuild/FBuildCore/BFF/BFFStackFrame.h"
#include "Tools/FBuild/FBuildCore/BFF/BFFVariable.h"
#include "Tools/FBuild/FBuildCore/Graph/AliasNode.h"
#include "Tools/FBuild/FBuildCore/Graph/CopyFileNode.h"
#include "Tools/FBuild/FBuildCore/Graph/NodeGraph.h"

// Core
#include "Core/FileIO/PathUtils.h"

// CONSTRUCTOR
//------------------------------------------------------------------------------
FunctionCopy::FunctionCopy()
: Function( "Copy" )
{
}

// AcceptsHeader
//------------------------------------------------------------------------------
/*virtual*/ bool FunctionCopy::AcceptsHeader() const
{
    return true;
}

// Commit
//------------------------------------------------------------------------------
/*virtual*/ bool FunctionCopy::Commit( NodeGraph & nodeGraph, const BFFToken * funcStartIter ) const
{
    // make sure all required variables are defined
    StackArray< AString > sources;
    const BFFVariable * dstFileV;
    if ( !GetStrings( funcStartIter, sources, ".Source", true ) ||
         !GetString( funcStartIter, dstFileV, ".Dest", true ) )
    {
        return false; // GetString will have emitted errors
    }

    // Optional
    AStackString<> sourceBasePath;
    if ( !GetString( funcStartIter, sourceBasePath, ".SourceBasePath", false ) )
    {
        return false; // GetString will have emitted errors
    }

    // Canonicalize the SourceBasePath
    if ( !sourceBasePath.IsEmpty() )
    {
        AStackString<> cleanValue;
        NodeGraph::CleanPath( sourceBasePath, cleanValue );
        PathUtils::EnsureTrailingSlash( cleanValue );
        sourceBasePath = cleanValue;
    }

    // check sources are not paths
    for ( const AString & srcFile : sources )
    {
        // source must be a file, not a  path
        if ( PathUtils::IsFolderPath( srcFile ) )
        {
            Error::Error_1105_PathNotAllowed( funcStartIter, this, ".Source", srcFile );
            return false;
        }
    }

    // Pre-build dependencies
    Dependencies preBuildDependencies;
    if ( !GetNodeList( nodeGraph, funcStartIter, ".PreBuildDependencies", preBuildDependencies, false ) )
    {
        return false; // GetNodeList will have emitted an error
    }
    Array< AString > preBuildDependencyNames;
    preBuildDependencyNames.SetCapacity( preBuildDependencies.GetSize() );
    for ( const Dependency & dep : preBuildDependencies )
    {
        preBuildDependencyNames.Append( dep.GetNode()->GetName() );
    }

    // get source node
    Array< Node * > srcNodes;
    {
        for ( const AString & source : sources )
        {

            Node * srcNode = nodeGraph.FindNode( source );
            if ( srcNode )
            {
                if ( GetSourceNodes( funcStartIter, srcNode, srcNodes ) == false )
                {
                    return false;
                }
            }
            else
            {
                // source file not defined by use - assume an external file
                srcNodes.Append( nodeGraph.CreateNode<FileNode>( source, funcStartIter ) );
            }
        }
    }

    AStackString<> dstFile;
    NodeGraph::CleanPath( dstFileV->GetString(), dstFile );
    const bool dstIsFolderPath = PathUtils::IsFolderPath( dstFile );

    // If source is multiple files, destination must be a path
    if ( ( srcNodes.GetSize() > 1 ) && ( dstIsFolderPath == false ) )
    {
        Error::Error_1400_CopyDestMissingSlash( funcStartIter, this );
        return false;
    }

    // make all the nodes for copies
    Dependencies copyNodes( srcNodes.GetSize() );
    for ( const Node * srcNode : srcNodes )
    {
        AStackString<> dst( dstFile );

        // dest can be a file OR a path.  If it's a path, use the source filename part
        if ( dstIsFolderPath )
        {
            // find filename part of source
            const AString & srcName = srcNode->GetName();

            // If the sourceBasePath is specified (and valid) use the name relative to that
            if ( !sourceBasePath.IsEmpty() && PathUtils::PathBeginsWith( srcName, sourceBasePath ) )
            {
                // Use everything relative to the SourceBasePath
                dst += srcName.Get() + sourceBasePath.GetLength();
            }
            else
            {
                // Use just the file name
                const char * lastSlash = srcName.FindLast( NATIVE_SLASH );
                dst += lastSlash ? ( lastSlash + 1 )    // append filename part if found
                                     : srcName.Get();   // otherwise append whole thing
            }
        }

        // check node doesn't already exist
        if ( const Node * existingNode = nodeGraph.FindNode( dst ) )
        {
            const BFFToken * existingToken = nodeGraph.FindNodeSourceToken( existingNode );
            Error::Error_1100_AlreadyDefined( funcStartIter, this, dst, existingToken );
            return false;
        }

        // create our node
        CopyFileNode * copyFileNode = nodeGraph.CreateNode<CopyFileNode>( dst, funcStartIter );
        copyFileNode->m_Source = srcNode->GetName();
        copyFileNode->m_PreBuildDependencyNames = preBuildDependencyNames;
        if ( !copyFileNode->Initialize( nodeGraph, funcStartIter, this ) )
        {
            return false; // Initialize will have emitted an error
        }

        copyNodes.Add( copyFileNode );
    }

    // handle alias creation
    return ProcessAlias( nodeGraph, copyNodes );
}

// GetSourceNodes
//------------------------------------------------------------------------------
bool FunctionCopy::GetSourceNodes( const BFFToken * iter, Node * node, Array< Node * > & nodes ) const
{
    if ( node->GetType() == Node::ALIAS_NODE )
    {
        // resolve aliases to real nodes
        const AliasNode * aliasNode = node->CastTo< AliasNode >();
        for ( const Dependency & dep : aliasNode->GetAliasedNodes() )
        {
            if ( !GetSourceNodes( iter, dep.GetNode(), nodes ) )
            {
                return false;
            }
        }
        return true;
    }
    else if ( node->IsAFile() )
    {
        // anything that results in a file is ok
        nodes.Append( node );
        return true;
    }

    // something we don't know how to handle
    Error::Error_1005_UnsupportedNodeType( iter, this, ".Source", node->GetName(), node->GetType() );
    return false;
}

//------------------------------------------------------------------------------
