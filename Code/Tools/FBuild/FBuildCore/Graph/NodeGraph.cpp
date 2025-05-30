// Node.cpp - base interface for dependency graph nodes
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "NodeGraph.h"

#include "Tools/FBuild/FBuildCore/BFF/BFFParser.h"
#include "Tools/FBuild/FBuildCore/BFF/Functions/FunctionSettings.h"
#include "Tools/FBuild/FBuildCore/FLog.h"
#include "Tools/FBuild/FBuildCore/FBuild.h"
#include "Tools/FBuild/FBuildCore/Graph/MetaData/Meta_IgnoreForComparison.h"
#include "Tools/FBuild/FBuildCore/WorkerPool/JobQueue.h"

#include "AliasNode.h"
#include "CompilerNode.h"
#include "CopyDirNode.h"
#include "CopyFileNode.h"
#include "CSNode.h"
#include "DirectoryListNode.h"
#include "DLLNode.h"
#include "ExeNode.h"
#include "ExecNode.h"
#include "FileNode.h"
#include "LibraryNode.h"
#include "ListDependenciesNode.h"
#include "ObjectListNode.h"
#include "ObjectNode.h"
#include "RemoveDirNode.h"
#include "SettingsNode.h"
#include "SLNNode.h"
#include "TestNode.h"
#include "TextFileNode.h"
#include "UnityNode.h"
#include "VCXProjectNode.h"
#include "VSProjectExternalNode.h"
#include "XCodeProjectNode.h"

// Core
#include "Core/Containers/UniquePtr.h"
#include "Core/Env/Env.h"
#include "Core/Env/ErrorFormat.h"
#include "Core/FileIO/ChainedMemoryStream.h"
#include "Core/FileIO/ConstMemoryStream.h"
#include "Core/FileIO/FileIO.h"
#include "Core/FileIO/FileStream.h"
#include "Core/FileIO/PathUtils.h"
#include "Core/Math/xxHash.h"
#include "Core/Mem/Mem.h"
#include "Core/Process/Thread.h"
#include "Core/Profile/Profile.h"
#include "Core/Reflection/ReflectedProperty.h"
#include "Core/Strings/AStackString.h"
#include "Core/Strings/LevenshteinDistance.h"
#include "Core/Tracing/Tracing.h"

#include <string.h>

// Defines
//------------------------------------------------------------------------------

// Static Data
//------------------------------------------------------------------------------
/*static*/ uint32_t NodeGraph::s_BuildPassTag( 0 );

// IsValid (NodeGraphHeader)
//------------------------------------------------------------------------------
bool NodeGraphHeader::IsValid() const
{
    // Check header token is valid
    if ( ( m_Identifier[ 0 ] != 'N' ) ||
         ( m_Identifier[ 1 ] != 'G' ) ||
         ( m_Identifier[ 2 ] != 'D' ) )
    {
        return false;
    }
    return true;
}

// CONSTRUCTOR
//------------------------------------------------------------------------------
NodeGraph::NodeGraph( unsigned nodeMapHashBits )
: m_NodeMapMaxKey( ( 1u << nodeMapHashBits ) - 1u )
, m_Settings( nullptr )
{
    m_AllNodes.SetCapacity( 1024 );
    m_UsedFiles.SetCapacity( 16 );

    ASSERT( nodeMapHashBits > 0 && nodeMapHashBits < 32 );
    m_NodeMap = FNEW_ARRAY( Node * [ m_NodeMapMaxKey + 1 ] );
    memset( m_NodeMap, 0, sizeof( Node * ) * ( m_NodeMapMaxKey + 1 ) );

    #if defined( ENABLE_FAKE_SYSTEM_FAILURE )
        // Ensure debug flag doesn't linger between test runs
        ASSERT( ObjectNode::GetFakeSystemFailureForNextJob() == false );
    #endif
}

// DESTRUCTOR
//------------------------------------------------------------------------------
NodeGraph::~NodeGraph()
{
    for ( Node * node : m_AllNodes )
    {
        FDELETE( node );
    }

    FDELETE_ARRAY( m_NodeMap );
}

// Initialize
//------------------------------------------------------------------------------
/* static*/ NodeGraph * NodeGraph::Initialize( const char * bffFile,
                                               const char * nodeGraphDBFile,
                                               bool forceMigration )
{
    PROFILE_FUNCTION;

    ASSERT( bffFile ); // must be supplied (or left as default)
    ASSERT( nodeGraphDBFile ); // must be supplied (or left as default)

    // Try to load the old DB
    NodeGraph * oldNG = FNEW( NodeGraph );
    LoadResult res = oldNG->Load( nodeGraphDBFile );

    // Tests can force us to do a migration even if the DB didn't change
    if ( forceMigration )
    {
        // If migration can't be forced, then the test won't function as expected
        // so we want to catch that failure.
        ASSERT( ( res == LoadResult::OK ) || ( res == LoadResult::OK_BFF_NEEDS_REPARSING ) );

        res = LoadResult::OK_BFF_NEEDS_REPARSING; // forces migration
    }

    // What happened?
    switch ( res )
    {
        case LoadResult::MISSING_OR_INCOMPATIBLE:
        case LoadResult::LOAD_ERROR:
        case LoadResult::LOAD_ERROR_MOVED:
        {
            // Failed due to moved DB?
            if ( res == LoadResult::LOAD_ERROR_MOVED )
            {
                // Is moving considered fatal?
                if ( FBuild::Get().GetOptions().m_ContinueAfterDBMove == false )
                {
                    // Corrupt DB or other fatal problem
                    FDELETE( oldNG );
                    return nullptr;
                }
            }

            // Failed due to corrupt DB? Make a backup to assist triage
            if ( res == LoadResult::LOAD_ERROR )
            {
                AStackString<> corruptDBName( nodeGraphDBFile );
                corruptDBName += ".corrupt";
                FileIO::FileMove( AStackString<>( nodeGraphDBFile ), corruptDBName ); // Will overwrite if needed
            }

            // Create a fresh DB by parsing the BFF
            FDELETE( oldNG );
            NodeGraph * newNG = FNEW( NodeGraph );
            if ( newNG->ParseFromRoot( bffFile ) == false )
            {
                FDELETE( newNG );
                return nullptr; // ParseFromRoot will have emitted an error
            }
            return newNG;
        }
        case LoadResult::OK_BFF_NEEDS_REPARSING:
        {
            // Create a fresh DB by parsing the modified BFF
            NodeGraph * newNG = FNEW( NodeGraph );
            if ( newNG->ParseFromRoot( bffFile ) == false )
            {
                FDELETE( newNG );
                FDELETE( oldNG );
                return nullptr;
            }

            // Migrate old DB info to new DB
            newNG->Migrate( *oldNG );
            FDELETE( oldNG );

            return newNG;
        }
        case LoadResult::OK:
        {
            // Nothing more to do
            return oldNG;
        }
    }

    ASSERT( false ); // Should not get here
    return nullptr;
}

// ParseFromRoot
//------------------------------------------------------------------------------
bool NodeGraph::ParseFromRoot( const char * bffFile )
{
    ASSERT( m_UsedFiles.IsEmpty() ); // NodeGraph cannot be recycled

    // re-parse the BFF from scratch, clean build will result
    BFFParser bffParser( *this );
    const bool ok = bffParser.ParseFromFile( bffFile );
    if ( ok )
    {
        // Store a pointer to the SettingsNode as defined by the BFF, or create a
        // default instance if needed.
        if ( m_Settings == nullptr )
        {
            // Create a default
            const AStackString<> settingsNodeName(  "$$Settings$$" );
            SettingsNode * settingsNode = CreateNode<SettingsNode>( settingsNodeName, &BFFToken::GetBuiltInToken() );
            settingsNode->Initialize( *this, &BFFToken::GetBuiltInToken(), nullptr );
            ASSERT( m_Settings ); // SettingsNode registers itself
        }

        // Parser will populate m_UsedFiles
        const Array<BFFFile *> & usedFiles = bffParser.GetUsedFiles();
        m_UsedFiles.SetCapacity( usedFiles.GetSize() );
        for ( const BFFFile * file : usedFiles )
        {
            m_UsedFiles.EmplaceBack( file->GetFileName(), file->GetTimeStamp(), file->GetHash() );
        }
    }

    // Free token tracking data we no longer need (and won't be valid when
    // BFFParser falls out of scope)
    m_NodeSourceTokens.Destruct();

    return ok;
}

// Load
//------------------------------------------------------------------------------
NodeGraph::LoadResult NodeGraph::Load( const char * nodeGraphDBFile )
{
    // Open previously saved DB
    FileStream fs;
    if ( fs.Open( nodeGraphDBFile, FileStream::READ_ONLY ) == false )
    {
        return LoadResult::MISSING_OR_INCOMPATIBLE;
    }

    // Read it into memory to avoid lots of tiny disk accesses
    const size_t fileSize = (size_t)fs.GetFileSize();
    UniquePtr< char, FreeDeletor > memory( (char *)ALLOC( fileSize ) );
    if ( fs.ReadBuffer( memory.Get(), fileSize ) != fileSize )
    {
        FLOG_ERROR( "Could not read Database. Error: %s File: '%s'", LAST_ERROR_STR, nodeGraphDBFile );
        return LoadResult::LOAD_ERROR;
    }
    ConstMemoryStream ms( memory.Get(), fileSize );

    // Load the Old DB
    const NodeGraph::LoadResult res = Load( ms, nodeGraphDBFile );
    if ( res == LoadResult::LOAD_ERROR )
    {
        FLOG_ERROR( "Database corrupt (clean build will occur): '%s'", nodeGraphDBFile );
    }
    return res;
}

// Load
//------------------------------------------------------------------------------
NodeGraph::LoadResult NodeGraph::Load( ConstMemoryStream & stream, const char * nodeGraphDBFile )
{
    bool compatibleDB;
    bool movedDB;
    Array< UsedFile > usedFiles;
    if ( ReadHeaderAndUsedFiles( stream, nodeGraphDBFile, usedFiles, compatibleDB, movedDB ) == false )
    {
        return movedDB ? LoadResult::LOAD_ERROR_MOVED : LoadResult::LOAD_ERROR;
    }

    // old or otherwise incompatible DB version?
    if ( !compatibleDB )
    {
        FLOG_WARN( "Database version has changed (clean build will occur)." );
        return LoadResult::MISSING_OR_INCOMPATIBLE;
    }

    // Take not of whether we need to reparse
    bool bffNeedsReparsing = false;

    // check if any files used have changed
    for ( size_t i=0; i<usedFiles.GetSize(); ++i )
    {
        const AString & fileName = usedFiles[ i ].m_FileName;
        const uint64_t timeStamp = FileIO::GetFileLastWriteTime( fileName );
        if ( timeStamp == usedFiles[ i ].m_TimeStamp )
        {
            continue; // timestamps match, no need to check hashes
        }

        FileStream fs;
        if ( fs.Open( fileName.Get(), FileStream::READ_ONLY ) == false )
        {
            if ( !bffNeedsReparsing )
            {
                FLOG_VERBOSE( "BFF file '%s' missing or unopenable (reparsing will occur).", fileName.Get() );
                bffNeedsReparsing = true;
            }
            continue; // not opening the file is not an error, it could be not needed anymore
        }

        const size_t size = (size_t)fs.GetFileSize();
        UniquePtr< void, FreeDeletor > mem( ALLOC( size ) );
        if ( fs.Read( mem.Get(), size ) != size )
        {
            return LoadResult::LOAD_ERROR; // error reading
        }

        const uint64_t dataHash = xxHash3::Calc64( mem.Get(), size );
        if ( dataHash == usedFiles[ i ].m_DataHash )
        {
            // file didn't change, update stored timestamp to save time on the next run
            usedFiles[ i ].m_TimeStamp = timeStamp;
            continue;
        }

        // Tell used reparsing will occur (Warn only about the first file)
        if ( !bffNeedsReparsing )
        {
            FLOG_WARN( "BFF file '%s' has changed (reparsing will occur).", fileName.Get() );
            bffNeedsReparsing = true;
        }
    }

    m_UsedFiles = usedFiles;

    // TODO:C The serialization of these settings doesn't really belong here (not part of node graph)

    // environment
    uint32_t envStringSize = 0;
    VERIFY( stream.Read( envStringSize ) );
    UniquePtr< char, FreeDeletor > envString;
    AStackString<> libEnvVar;
    if ( envStringSize > 0 )
    {
        envString = ( (char *)ALLOC( envStringSize ) );
        VERIFY( stream.Read( envString.Get(), envStringSize ) );
        VERIFY( stream.Read( libEnvVar ) );
    }

    // imported environment variables
    uint32_t importedEnvironmentsVarsSize = 0;
    VERIFY( stream.Read( importedEnvironmentsVarsSize ) );
    if ( importedEnvironmentsVarsSize > 0 )
    {
        AStackString<> varName;
        AStackString<> varValue;
        uint32_t savedVarHash = 0;
        uint32_t importedVarHash = 0;

        for ( uint32_t i = 0; i < importedEnvironmentsVarsSize; ++i )
        {
            VERIFY( stream.Read( varName ) );
            VERIFY( stream.Read( savedVarHash ) );

            const bool optional = ( savedVarHash == 0 ); // a hash of 0 means the env var was missing when it was evaluated
            if ( FBuild::Get().ImportEnvironmentVar( varName.Get(), optional, varValue, importedVarHash ) == false )
            {
                // make sure the user knows why some things might re-build (only the first thing warns)
                if ( !bffNeedsReparsing )
                {
                    FLOG_WARN( "'%s' Environment variable was not found - BFF will be re-parsed\n", varName.Get() );
                    bffNeedsReparsing = true;
                }
            }
            if ( importedVarHash != savedVarHash )
            {
                // make sure the user knows why some things might re-build (only the first thing warns)
                if ( !bffNeedsReparsing )
                {
                    FLOG_WARN( "'%s' Environment variable has changed - BFF will be re-parsed\n", varName.Get() );
                    bffNeedsReparsing = true;
                }
            }
        }
    }

    // check if 'LIB' env variable has changed
    uint32_t libEnvVarHashInDB( 0 );
    VERIFY( stream.Read( libEnvVarHashInDB ) );
    {
        // If the Environment will be overriden, make sure we use the LIB from that
        const uint32_t libEnvVarHash = ( envStringSize > 0 ) ? xxHash::Calc32( libEnvVar ) : GetLibEnvVarHash();
        if ( libEnvVarHashInDB != libEnvVarHash )
        {
            // make sure the user knows why some things might re-build (only the first thing warns)
            if ( !bffNeedsReparsing )
            {
                FLOG_WARN( "'%s' Environment variable has changed - BFF will be re-parsed\n", "LIB" );
                bffNeedsReparsing = true;
            }
        }
    }

    // Files use in file_exists checks
    BFFFileExists fileExistsInfo;
    fileExistsInfo.Load( stream );
    bool added;
    const AString * changedFile = fileExistsInfo.CheckForChanges( added );
    if ( changedFile )
    {
        FLOG_WARN( "File used in file_exists was %s '%s' - BFF will be re-parsed\n", added ? "added" : "removed", changedFile->Get() );
        bffNeedsReparsing = true;
    }

    ASSERT( m_AllNodes.GetSize() == 0 );

    // Read nodes
    uint32_t numNodes;
    VERIFY( stream.Read( numNodes ) );
    m_AllNodes.SetCapacity( numNodes );
    for ( uint32_t i = 0; i < numNodes; ++i )
    {
        // Load each node
        const Node * const n = Node::Load( *this, stream );
        ASSERT( m_AllNodes[ i ] == n ); // Array is populated as loaded
        (void)n;
    }
    for ( Node * node : m_AllNodes )
    {
        // Load dependencies, but not for FileNodes which have none
        if ( node->GetType() != Node::FILE_NODE )
        {
            Node::LoadDependencies( *this, node, stream );
        }
    }
    for ( Node * node : m_AllNodes )
    {
        // Dispatch post-load callback
        if ( node->GetType() != Node::FILE_NODE )
        {
            node->PostLoad( *this ); // TODO:C Eliminate the need for this
        }
    }

    m_Settings = FindNode( AStackString<>( "$$Settings$$" ) )->CastTo< SettingsNode >();
    ASSERT( m_Settings );

    if ( bffNeedsReparsing )
    {
        return LoadResult::OK_BFF_NEEDS_REPARSING;
    }

    // Everything OK - propagate global settings
    //------------------------------------------------

    // file_exists files
    FBuild::Get().GetFileExistsInfo() = fileExistsInfo;

    // Environment
    if ( envStringSize > 0 )
    {
        FBuild::Get().SetEnvironmentString( envString.Get(), envStringSize, libEnvVar );
    }

    return LoadResult::OK;
}

// Save
//------------------------------------------------------------------------------
void NodeGraph::Save( ChainedMemoryStream & stream, const char* nodeGraphDBFile ) const
{
    // write header and version
    const NodeGraphHeader header;
    stream.Write( (const void *)&header, sizeof( header ) );

    AStackString<> nodeGraphDBFileClean( nodeGraphDBFile );
    NodeGraph::CleanPath( nodeGraphDBFileClean );
    stream.Write( nodeGraphDBFileClean );

    // write used file
    const uint32_t numUsedFiles = (uint32_t)m_UsedFiles.GetSize();
    stream.Write( numUsedFiles );

    for ( const NodeGraph::UsedFile & usedFile : m_UsedFiles )
    {
        stream.Write( usedFile.m_FileName );
        stream.Write( usedFile.m_TimeStamp );
        stream.Write( usedFile.m_DataHash );
    }

    // TODO:C The serialization of these settings doesn't really belong here (not part of node graph)
    {
        // environment
        const uint32_t envStringSize = FBuild::Get().GetEnvironmentStringSize();
        stream.Write( envStringSize );
        if ( envStringSize > 0 )
        {
            const char * envString = FBuild::Get().GetEnvironmentString();
            stream.Write( envString, envStringSize );

            AStackString<> libEnvVar;
            FBuild::Get().GetLibEnvVar( libEnvVar );
            stream.Write( libEnvVar );
        }

        // imported environment variables
        const Array< FBuild::EnvironmentVarAndHash > & importedEnvironmentsVars = FBuild::Get().GetImportedEnvironmentVars();
        const uint32_t importedEnvironmentsVarsSize = static_cast<uint32_t>( importedEnvironmentsVars.GetSize() );
        ASSERT( importedEnvironmentsVarsSize == importedEnvironmentsVars.GetSize() );
        stream.Write( importedEnvironmentsVarsSize );
        for ( const FBuild::EnvironmentVarAndHash & varAndHash : importedEnvironmentsVars )
        {
            stream.Write( varAndHash.GetName() );
            stream.Write( varAndHash.GetHash() );
        }

        // 'LIB' env var hash
        const uint32_t libEnvVarHash = GetLibEnvVarHash();
        stream.Write( libEnvVarHash );
    }

    // Write file_exists tracking info
    FBuild::Get().GetFileExistsInfo().Save( stream );

    // Write nodes
    const size_t numNodes = m_AllNodes.GetSize();
    stream.Write( (uint32_t)numNodes );
    uint32_t index = 0;
    for ( const Node * node : m_AllNodes )
    {
        // Save each node
        Node::Save( stream, node );
        node->SetBuildPassTag( index++ ); // Save index for dependency serialization
    }
    for ( const Node * node : m_AllNodes )
    {
        // Save dependencies, but not for FileNodes which have none
        if ( node->GetType() != Node::FILE_NODE )
        {
            Node::SaveDependencies( stream, node );
        }
    }

    // Calculate hash of stream excluding header
    {
        NodeGraphHeader * headerToUpdate = nullptr;

        // Calculate hash of non-contiguous pages
        xxHash3Accumulator accumulator;
        for ( uint32_t i = 0; i < stream.GetNumPages(); ++i )
        {
            uint32_t dataSize = 0;
            char * data = stream.GetPage( i, dataSize );
            if ( i == 0 )
            {
                // Exclude header from first page
                headerToUpdate = reinterpret_cast<NodeGraphHeader *>( data );
                ASSERT( dataSize >= sizeof(NodeGraphHeader) );
                data += sizeof(NodeGraphHeader);
                dataSize -= sizeof(NodeGraphHeader);
            }

            accumulator.AddData( data, dataSize );
        }
        const uint64_t hash = accumulator.Finalize64();

        // Update hash in header
        ASSERT( headerToUpdate ); // Guaranteed to be in first page
        ASSERT( headerToUpdate->GetContentHash() == 0 );
        headerToUpdate->SetContentHash( hash );
    }
}

// SerializeToText
//------------------------------------------------------------------------------
void NodeGraph::SerializeToText( const Dependencies & deps, AString & outBuffer ) const
{
    s_BuildPassTag++;

    if ( deps.IsEmpty() == false )
    {
        for ( const Dependency & dep : deps )
        {
            SerializeToText( dep.GetNode(), 0, outBuffer );
        }
    }
    else
    {
        for ( Node * node : m_AllNodes )
        {
            SerializeToText( node, 0, outBuffer );
        }
    }
}

// SerializeToText
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::SerializeToText( Node * node, uint32_t depth, AString & outBuffer )
{
    // Print this even if it has been visited before so the edge is visible
    outBuffer.AppendFormat( "%*s%s %s\n", depth * 4, "", node->GetTypeName(), node->GetName().Get() );

    // Don't descend into already visited nodes
    if ( node->GetBuildPassTag() == s_BuildPassTag )
    {
        if ( node->GetPreBuildDependencies().GetSize() ||
             node->GetStaticDependencies().GetSize() ||
             node->GetDynamicDependencies().GetSize() )
        {
            outBuffer.AppendFormat( "%*s...\n", ( depth + 1 ) * 4, "" );
        }
        return;
    }
    node->SetBuildPassTag( s_BuildPassTag );

    // Dependencies
    SerializeToText( "PreBuild", node->GetPreBuildDependencies(), depth, outBuffer );
    SerializeToText( "Static", node->GetStaticDependencies(), depth, outBuffer );
    SerializeToText( "Dynamic", node->GetDynamicDependencies(), depth, outBuffer );
}

// SerializeToText
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::SerializeToText( const char * title, const Dependencies & dependencies, uint32_t depth, AString & outBuffer )
{
    if ( dependencies.IsEmpty() == false )
    {
        outBuffer.AppendFormat( "%*s%s\n", depth * 4 + 2, "", title );
        for ( const Dependency & dep : dependencies )
        {
            SerializeToText( dep.GetNode(), depth + 1, outBuffer );
        }
    }
}

// SerializeToDotFormat
//------------------------------------------------------------------------------
void NodeGraph::SerializeToDotFormat( const Dependencies & deps,
                                      const bool fullGraph,
                                      AString & outBuffer ) const
{
    s_BuildPassTag++; // Used to mark nodes as we sweep to visit each node only once

    // Header of DOT format
    outBuffer += "digraph G\n";
    outBuffer += "{\n";
    outBuffer += "\trankdir=LR\n";
    outBuffer += "\tnode [shape=record;style=filled]\n";

    if ( deps.IsEmpty() == false )
    {
        // Emit subset of graph for specified targets
        for ( const Dependency & dep : deps )
        {
            SerializeToDot( dep.GetNode(), fullGraph, outBuffer );
        }
    }
    else
    {
        // Emit entire graph
        for ( Node * node : m_AllNodes )
        {
            SerializeToDot( node, fullGraph, outBuffer );
        }
    }

    // Footer of DOT format
    outBuffer += "}\n";
}

// SerializeToDot
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::SerializeToDot( Node * node,
                                                 const bool fullGraph,
                                                 AString & outBuffer )
{
    // Early out for nodes we've already visited
    if ( node->GetBuildPassTag() == s_BuildPassTag )
    {
        return;
    }
    node->SetBuildPassTag( s_BuildPassTag ); // Mark as visited

    // If outputting a reduced graph, prune out any leaf FileNodes.
    // (i.e. files that exist outside of the build - typically source code files)
    const bool isLeafFileNode = ( node->GetType() == Node::FILE_NODE );
    if ( isLeafFileNode && ( fullGraph == false ) )
    {
        return; // Strip this node
    }

    // Name of this node
    AStackString<> name( node->GetName() );
    name.Replace( "\\", "\\\\" ); // Escape slashes in this name
    outBuffer.AppendFormat( "\n\t\"%s\" %s // %s\n",
                            name.Get(),
                            isLeafFileNode ? "[style=none]" : "",
                            node->GetTypeName() );

    // Dependencies
    SerializeToDot( "PreBuild", "[style=dashed]", node, node->GetPreBuildDependencies(), fullGraph, outBuffer );
    SerializeToDot( "Static", nullptr, node, node->GetStaticDependencies(), fullGraph, outBuffer );
    SerializeToDot( "Dynamic", "[color=gray]", node, node->GetDynamicDependencies(), fullGraph, outBuffer );

    // Recurse into Dependencies
    SerializeToDot( node->GetPreBuildDependencies(), fullGraph, outBuffer );
    SerializeToDot( node->GetStaticDependencies(), fullGraph, outBuffer );
    SerializeToDot( node->GetDynamicDependencies(), fullGraph, outBuffer );
}

// SerializeToDot
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::SerializeToDot( const char * dependencyType,
                                                 const char * style,
                                                 const Node * node,
                                                 const Dependencies & dependencies,
                                                 const bool fullGraph,
                                                 AString & outBuffer )
{
    if ( dependencies.IsEmpty() )
    {
        return;
    }

    // Escape slashes in this name
    AStackString<> left( node->GetName() );
    left.Replace( "\\", "\\\\" );

    // All the dependencies
    for ( const Dependency & dep : dependencies )
    {
        // If outputting a reduced graph, prune out links to leaf FileNodes.
        // (i.e. files that exist outside of the build - typically source code files)
        if ( ( fullGraph == false ) && ( dep.GetNode()->GetType() == Node::FILE_NODE ) )
        {
            continue;
        }

        // Write the graph edge
        AStackString<> right( dep.GetNode()->GetName() );
        right.Replace( "\\", "\\\\" );
        outBuffer.AppendFormat( "\t\t/*%-8s*/ \"%s\" -> \"%s\"",
                                dependencyType,
                                left.Get(),
                                right.Get() );

        // Append the optional style
        if ( style )
        {
            outBuffer += ' ';
            outBuffer += style;
        }

        // Terminate the line
        outBuffer += '\n';
    }
}

// SerializeToDot
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::SerializeToDot( const Dependencies & dependencies,
                                                 const bool fullGraph,
                                                 AString & outBuffer )
{
    for ( const Dependency & dep : dependencies )
    {
        SerializeToDot( dep.GetNode(), fullGraph, outBuffer );
    }
}

// FindNode (AString &)
//------------------------------------------------------------------------------
Node * NodeGraph::FindNode( const AString & nodeName ) const
{
    // try to find node 'as is'
    Node * n = FindNodeInternal( nodeName, 0 );
    if ( n )
    {
        return n;
    }

    // the expanding to a full path
    AStackString< 1024 > fullPath;
    CleanPath( nodeName, fullPath );
    return FindNodeInternal( fullPath, 0 );
}

// FindNodeExact (AString &)
//------------------------------------------------------------------------------
Node * NodeGraph::FindNodeExact( const AString & nodeName ) const
{
    // try to find node 'as is'
    return FindNodeInternal( nodeName, 0 );
}

// GetNodeByIndex
//------------------------------------------------------------------------------
Node * NodeGraph::GetNodeByIndex( size_t index ) const
{
    Node * n = m_AllNodes[ index ];
    ASSERT( n );
    return n;
}

//GetNodeCount
//-----------------------------------------------------------------------------
size_t NodeGraph::GetNodeCount() const
{
    return m_AllNodes.GetSize();
}

//------------------------------------------------------------------------------
void NodeGraph::SetSettings( const SettingsNode & settings )
{
    ASSERT( m_Settings == nullptr ); // Should only be called once
    m_Settings = &settings;
}

// RegisterNode
//------------------------------------------------------------------------------
void NodeGraph::RegisterNode( Node * node, const BFFToken * sourceToken )
{
    ASSERT( Thread::IsMainThread() );
    ASSERT( node->GetName().IsEmpty() == false );
    ASSERT( FindNode( node->GetName() ) == nullptr );
    AddNode( node );
    RegisterSourceToken( node, sourceToken );
}

// RegisterSourceToken
//------------------------------------------------------------------------------
void NodeGraph::RegisterSourceToken( const Node * node, const BFFToken * sourceToken )
{
    // Where available, record the source token for the node
    if ( sourceToken )
    {
        // Amortize array growth in parallel with m_AllNodes
        m_NodeSourceTokens.SetCapacity( m_AllNodes.GetCapacity() );

        // Nobody should have added this before
        ASSERT( m_NodeSourceTokens.GetSize() < m_AllNodes.GetSize() );

        // Array may be non-contiguous, so fill it in
        while ( m_NodeSourceTokens.GetSize() < m_AllNodes.GetSize() )
        {
            m_NodeSourceTokens.EmplaceBack( nullptr );
        }

        // Store the token in the parallel at the same place as the node
        ASSERT( m_AllNodes.Top() == node ); (void)node;
        m_NodeSourceTokens.Top() = sourceToken;
    }
}

// CreateNode
//------------------------------------------------------------------------------
Node * NodeGraph::CreateNode( Node::Type type, AString && name, uint32_t nameHash )
{
    ASSERT( Thread::IsMainThread() );

    // Ensure provided hash is correct
    ASSERT( nameHash == Node::CalcNameHash( name ) );

    Node * node = nullptr;
    switch ( type )
    {
        case Node::PROXY_NODE:              ASSERT( false ); return nullptr;
        case Node::COPY_FILE_NODE:          node = FNEW( CopyFileNode() ); break;
        case Node::DIRECTORY_LIST_NODE:     node = FNEW( DirectoryListNode() ); break;
        case Node::EXEC_NODE:               node = FNEW( ExecNode() ); break;
        case Node::FILE_NODE:
        {
            node = FNEW( FileNode() );
            node->m_ControlFlags = Node::FLAG_ALWAYS_BUILD; // TODO:C Eliminate special case
            break;
        }
        case Node::LIBRARY_NODE:            node = FNEW( LibraryNode() ); break;
        case Node::OBJECT_NODE:             node = FNEW( ObjectNode() ); break;
        case Node::ALIAS_NODE:              node = FNEW( AliasNode() ); break;
        case Node::EXE_NODE:                node = FNEW( ExeNode() ); break;
        case Node::CS_NODE:                 node = FNEW( CSNode() ); break;
        case Node::UNITY_NODE:              node = FNEW( UnityNode() ); break;
        case Node::TEST_NODE:               node = FNEW( TestNode() ); break;
        case Node::COMPILER_NODE:           node = FNEW( CompilerNode() ); break;
        case Node::DLL_NODE:                node = FNEW( DLLNode() ); break;
        case Node::VCXPROJECT_NODE:         node = FNEW( VCXProjectNode() ); break;
        case Node::VSPROJEXTERNAL_NODE:     node = FNEW( VSProjectExternalNode() ); break;
        case Node::OBJECT_LIST_NODE:        node = FNEW( ObjectListNode() ); break;
        case Node::COPY_DIR_NODE:           node = FNEW( CopyDirNode() ); break;
        case Node::SLN_NODE:                node = FNEW( SLNNode() ); break;
        case Node::REMOVE_DIR_NODE:         node = FNEW( RemoveDirNode() ); break;
        case Node::XCODEPROJECT_NODE:       node = FNEW( XCodeProjectNode() ); break;
        case Node::SETTINGS_NODE:           node = FNEW( SettingsNode() ); break;
        case Node::TEXT_FILE_NODE:          node = FNEW( TextFileNode() ); break;
        case Node::LIST_DEPENDENCIES_NODE:  node = FNEW( ListDependenciesNode() ); break;
        case Node::NUM_NODE_TYPES:          ASSERT( false ); return nullptr;
    }

    ASSERT( node ); // All cases handled above means this is impossible

    // Names for files must be normalized by the time we get here
    ASSERT( !node->IsAFile() || IsCleanPath( name ) );

    // Store name and track new node
    node->SetName( Move( name ), nameHash );
    AddNode( node );

    return node;
}


// CreateNode
//------------------------------------------------------------------------------
Node * NodeGraph::CreateNode( Node::Type type, const AString & name, const BFFToken * sourceToken )
{
    // Where possible callers should call the move version to transfer ownership
    // of strings, but callers don't always have a string to transfer so this
    // helper can be called in those situations
    AString nameCopy;

    // TODO:C Eliminate special case handling of FileNode
    // For historical reasons users of FileNodes don't clean paths so we have to
    // do it here. Ideally this special case would be eliminated in the future.
    if ( type == Node::FILE_NODE )
    {
        // Clean path
        AStackString< 512 > cleanPath;
        CleanPath( name, cleanPath );
        nameCopy = cleanPath;
    }
    else
    {
        nameCopy = name;
    }

    const uint32_t nameHash = Node::CalcNameHash( nameCopy );
    Node * node = CreateNode( type, Move( nameCopy ), nameHash );

    RegisterSourceToken( node, sourceToken );

    return node;
}

// AddNode
//------------------------------------------------------------------------------
void NodeGraph::AddNode( Node * node )
{
    ASSERT( Thread::IsMainThread() );

    ASSERT( node );

    ASSERT( node->GetNameHash() == Node::CalcNameHash( node->GetName() ) );
    ASSERT( FindNodeInternal( node->GetName(), node->GetNameHash() ) == nullptr ); // node name must be unique

    // track in NodeMap
    const size_t key = ( node->GetNameHash() & m_NodeMapMaxKey );
    node->m_Next = m_NodeMap[ key ];
    m_NodeMap[ key ] = node;

    // add to list
    m_AllNodes.Append( node );
}

// Build
//------------------------------------------------------------------------------
void NodeGraph::DoBuildPass( Node * nodeToBuild )
{
    PROFILE_FUNCTION;

    s_BuildPassTag++;

    if ( nodeToBuild->GetType() == Node::PROXY_NODE )
    {
        const size_t total = nodeToBuild->GetStaticDependencies().GetSize();
        size_t failedCount = 0;
        size_t upToDateCount = 0;
        for ( const Dependency & dep : nodeToBuild->GetStaticDependencies() )
        {
            Node * n = dep.GetNode();
            if ( n->GetState() < Node::BUILDING )
            {
                BuildRecurse( n, 0 );
            }

            // check result of recursion (which may or may not be complete)
            if ( n->GetState() == Node::UP_TO_DATE )
            {
                upToDateCount++;
            }
            else if ( n->GetState() == Node::FAILED )
            {
                failedCount++;
            }
        }

        // only mark as failed or completed when all children have reached their final state
        if ( ( upToDateCount + failedCount ) == total )
        {
            // finished - mark with overall state
            nodeToBuild->SetState( failedCount ? Node::FAILED : Node::UP_TO_DATE );
        }
    }
    else
    {
        if ( nodeToBuild->GetState() < Node::BUILDING )
        {
            BuildRecurse( nodeToBuild, 0 );
        }
    }

    // Check for cyclic dependencies discoverable only at runtime
    if ( CheckForCyclicDependencies( nodeToBuild ) )
    {
        FBuild::AbortBuild();
    }

    // Make available all the jobs we discovered in this pass
    ASSERT( m_Settings );
    JobQueue::Get().FlushJobBatch( *m_Settings );
}

// BuildRecurse
//------------------------------------------------------------------------------
void NodeGraph::BuildRecurse( Node * nodeToBuild, uint32_t cost )
{
    ASSERT( nodeToBuild );

    // accumulate recursive cost
    cost += nodeToBuild->GetLastBuildTime();

    // False positive "Unannotated fallthrough between switch labels" (VS 2019 v14.29.30037)
    #if defined( _MSC_VER ) && ( _MSC_VER < 1935 )
        PRAGMA_DISABLE_PUSH_MSVC(26819)
    #endif

    switch ( nodeToBuild->GetState() )
    {
        case Node::NOT_PROCESSED:
        {
            // check pre-build dependencies
            const bool allDependenciesUpToDate = CheckDependencies( nodeToBuild, nodeToBuild->GetPreBuildDependencies(), cost );
            if ( allDependenciesUpToDate == false )
            {
                return; // not ready or failed
            }
            nodeToBuild->SetState( Node::STATIC_DEPS );

            [[fallthrough]];
        }
        case Node::STATIC_DEPS:
        {
            // check static dependencies
            const bool allDependenciesUpToDate = CheckDependencies( nodeToBuild, nodeToBuild->GetStaticDependencies(), cost );
            if ( allDependenciesUpToDate == false )
            {
                return; // not ready or failed
            }

            // If static deps require us to rebuild, dynamic dependencies need regenerating
            if ( FBuild::Get().GetOptions().m_ForceCleanBuild ||
                 nodeToBuild->DetermineNeedToBuildStatic() )
            {
                // Explicitly mark node in a way that will result in it rebuilding should
                // we cancel the build before building this node
                if ( nodeToBuild->m_Stamp == 0 )
                {
                    // Note that this is the first time we're building (since Node can't check
                    // stamp as we clear it below)
                    nodeToBuild->SetStatFlag( Node::STATS_FIRST_BUILD );
                }
                nodeToBuild->m_Stamp = 0;

                // Regenerate dynamic dependencies
                nodeToBuild->m_DynamicDependencies.Clear();
                if ( nodeToBuild->DoDynamicDependencies( *this ) == false )
                {
                    nodeToBuild->SetState( Node::FAILED );
                    return;
                }

                // Continue through to check dynamic dependencies and build
            }

            // Dynamic dependencies are ready to be checked
            nodeToBuild->SetState( Node::DYNAMIC_DEPS );

            [[fallthrough]];
        }
        case Node::DYNAMIC_DEPS:
        {
            // check dynamic dependencies
            const bool allDependenciesUpToDate = CheckDependencies( nodeToBuild, nodeToBuild->GetDynamicDependencies(), cost );
            if ( allDependenciesUpToDate == false )
            {
                return; // not ready or failed
            }

            // dependencies are uptodate, so node can now tell us if it needs
            // building
            nodeToBuild->SetStatFlag( Node::STATS_PROCESSED );
            if ( ( nodeToBuild->GetStamp() == 0 ) || // Avoid redundant work in DetermineNeedToBuild
                 nodeToBuild->DetermineNeedToBuildDynamic() )
            {
                nodeToBuild->m_RecursiveCost = cost;
                JobQueue::Get().AddJobToBatch( nodeToBuild );
            }
            else
            {
                if ( FLog::ShowVerbose() )
                {
                    FLOG_BUILD_REASON( "Up-To-Date '%s'\n", nodeToBuild->GetName().Get() );
                }
                nodeToBuild->SetState( Node::UP_TO_DATE );
            }
            break;
        }
        case Node::BUILDING:
        case Node::FAILED:
        case Node::UP_TO_DATE:
        {
            ASSERT(false); // Should be impossible
            break;
        }
    }

    // False positive "Unannotated fallthrough between switch labels" (VS 2019 v14.29.30037)
    #if defined( _MSC_VER ) && ( _MSC_VER < 1935 )
        PRAGMA_DISABLE_POP_MSVC // 26819
    #endif
}

// CheckDependencies
//------------------------------------------------------------------------------
bool NodeGraph::CheckDependencies( Node * nodeToBuild, const Dependencies & dependencies, uint32_t cost )
{
    ASSERT( nodeToBuild->GetType() != Node::PROXY_NODE );

    const uint32_t passTag = s_BuildPassTag;

    bool allDependenciesUpToDate = true;
    uint32_t numberNodesUpToDate = 0;
    uint32_t numberNodesFailed = 0;
    const bool stopOnFirstError = FBuild::Get().GetOptions().m_StopOnFirstError;

    for ( const Dependency & dep : dependencies )
    {
        Node * n = dep.GetNode();

        Node::State state = n->GetState();

        // recurse into nodes which have not been processed yet
        if ( state < Node::BUILDING )
        {
            // early out if already seen
            if ( n->GetBuildPassTag() != passTag )
            {
                // prevent multiple recursions in this pass
                n->SetBuildPassTag( passTag );

                BuildRecurse( n, cost );
            }
        }

        // dependency is uptodate, nothing more to be done
        state = n->GetState();
        if ( state == Node::UP_TO_DATE )
        {
            ++numberNodesUpToDate;
            continue;
        }

        if ( state == Node::BUILDING )
        {
            // ensure deepest traversal cost is kept
            if ( cost > nodeToBuild->m_RecursiveCost )
            {
                nodeToBuild->m_RecursiveCost = cost;
            }
        }

        allDependenciesUpToDate = false;

        // dependency failed?
        if ( state == Node::FAILED )
        {
            ++numberNodesFailed;
            if ( stopOnFirstError )
            {
                // propagate failure state to this node
                nodeToBuild->SetState( Node::FAILED );
                break;
            }
        }

        // keep trying to progress other nodes...
    }

    if ( !stopOnFirstError )
    {
        if ( ( numberNodesFailed + numberNodesUpToDate ) == dependencies.GetSize() )
        {
            if ( numberNodesFailed > 0 )
            {
                nodeToBuild->SetState( Node::FAILED );
            }
        }
    }

    return allDependenciesUpToDate;
}

//------------------------------------------------------------------------------
void NodeGraph::SetBuildPassTagForAllNodes( uint32_t value ) const
{
    for ( const Node * node : m_AllNodes )
    {
        node->SetBuildPassTag( value );
    }
}

// FindNodeSourceToken
//------------------------------------------------------------------------------
const BFFToken * NodeGraph::FindNodeSourceToken( const Node * node ) const
{
    // Find the index of the node. This is a slow operation, but we only expect
    // this to happen in non-critical paths like when emitting BFF parsing errors
    const size_t index = m_AllNodes.GetIndexOf( m_AllNodes.Find( node ) );

    // Return token if available. Not all nodes have creation info available.
    if ( index < m_NodeSourceTokens.GetSize() )
    {
        return m_NodeSourceTokens[ index ];
    }

    return nullptr;
}

// CleanPath
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::CleanPath( AString & name, bool makeFullPath )
{
    AStackString<> nameCopy( name );
    CleanPath( nameCopy, name, makeFullPath );
}

// CleanPath
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::CleanPath( const AString & name, AString & cleanPath, bool makeFullPath )
{
    ASSERT( &name != &cleanPath );

    char * dst;

    //  - path can be fully qualified
    bool isFullPath = PathUtils::IsFullPath( name );
    if ( !isFullPath && makeFullPath )
    {
        // make a full path by prepending working dir
        const AString & workingDir = FBuild::IsValid() ? FBuild::Get().GetWorkingDir() : AString::GetEmpty();

        // we're making the assumption that we don't need to clean the workingDir
        ASSERT( workingDir.Find( OTHER_SLASH ) == nullptr ); // bad slashes removed
        ASSERT( workingDir.Find( NATIVE_DOUBLE_SLASH ) == nullptr ); // redundant slashes removed

        // build the start of the path
        cleanPath = workingDir;
        if ( cleanPath.IsEmpty() == false )
        {
            cleanPath += NATIVE_SLASH;
        }

        // concatenate
        uint32_t len = cleanPath.GetLength();

        // make sure the dest will be big enough for the extra stuff
        cleanPath.SetLength( cleanPath.GetLength() + name.GetLength() );

        // set the output (which maybe a newly allocated ptr)
        dst = cleanPath.Get() + len;

        isFullPath = true;
    }
    else
    {
        // make sure the dest will be big enough
        cleanPath.SetLength( name.GetLength() );

        // copy from the start
        dst = cleanPath.Get();
    }

    // the untrusted part of the path we need to copy/fix
    const char * src = name.Get();
    const char * const srcEnd = name.GetEnd();

    // clean slashes
    char lastChar = NATIVE_SLASH; // consider first item to follow a path (so "..\file.dat" works)
    #if defined( __WINDOWS__ )
        while ( ( *src == NATIVE_SLASH ) || ( *src == OTHER_SLASH ) ) { ++src; } // strip leading slashes
    #endif

    const char * lowestRemovableChar = cleanPath.Get();
    if ( isFullPath )
    {
        #if defined( __WINDOWS__ )
            lowestRemovableChar += 3; // e.g. "c:\"
        #else
            lowestRemovableChar += 1; // e.g. "/"
        #endif
    }

    while ( src < srcEnd )
    {
        const char thisChar = *src;

        // hit a slash?
        if ( ( thisChar == NATIVE_SLASH ) || ( thisChar == OTHER_SLASH ) )
        {
            // write it the correct way
            *dst = NATIVE_SLASH;
            dst++;

            // skip until non-slashes
            while ( ( *src == NATIVE_SLASH ) || ( *src == OTHER_SLASH ) )
            {
                src++;
            }
            lastChar = NATIVE_SLASH;
            continue;
        }
        else if ( thisChar == '.' )
        {
            if ( lastChar == NATIVE_SLASH ) // fixed up slash, so we only need to check backslash
            {
                // check for \.\ (or \./)
                char nextChar = *( src + 1 );
                if ( ( nextChar == NATIVE_SLASH ) || ( nextChar == OTHER_SLASH ) || ( nextChar == '\0' ) )
                {
                    src++; // skip . and slashes
                    while ( ( *src == NATIVE_SLASH ) || ( *src == OTHER_SLASH ) )
                    {
                        ++src;
                    }
                    continue; // leave lastChar as-is, since we added nothing
                }

                // check for \..\ (or \../)
                if ( nextChar == '.' )
                {
                    nextChar = *( src + 2 );
                    if ( ( nextChar == NATIVE_SLASH ) || ( nextChar == OTHER_SLASH ) || ( nextChar == '\0' ) )
                    {
                        src+=2; // skip .. and slashes
                        while ( ( *src == NATIVE_SLASH ) || ( *src == OTHER_SLASH ) )
                        {
                            ++src;
                        }

                        if ( dst > lowestRemovableChar )
                        {
                            --dst; // remove slash

                            while ( dst > lowestRemovableChar ) // e.g. "c:\"
                            {
                                --dst;
                                if ( *dst == NATIVE_SLASH ) // only need to check for cleaned slashes
                                {
                                    ++dst; // keep this slash
                                    break;
                                }
                            }
                        }
                        else if ( !isFullPath )
                        {
                            *dst++ = '.';
                            *dst++ = '.';
                            *dst++ = NATIVE_SLASH;
                            lowestRemovableChar = dst;
                        }

                        continue;
                    }
                }
            }
        }

        // write non-slash character
        *dst++ = *src++;
        lastChar = thisChar;
    }

    // correct length of destination
    cleanPath.SetLength( (uint16_t)( dst - cleanPath.Get() ) );
    ASSERT( AString::StrLen( cleanPath.Get() ) == cleanPath.GetLength() );

    // sanity checks
    ASSERT( cleanPath.Find( OTHER_SLASH ) == nullptr ); // bad slashes removed
    ASSERT( cleanPath.Find( NATIVE_DOUBLE_SLASH ) == nullptr ); // redundant slashes removed
}

// FindNodeInternal
//------------------------------------------------------------------------------
Node * NodeGraph::FindNodeInternal( const AString & name, uint32_t nameHashHint ) const
{
    ASSERT( Thread::IsMainThread() );
    ASSERT( ( nameHashHint == 0 ) || ( nameHashHint == Node::CalcNameHash( name ) ) );

    const uint32_t hash = nameHashHint ? nameHashHint : Node::CalcNameHash( name );
    const size_t key = ( hash & m_NodeMapMaxKey );

    Node * n = m_NodeMap[ key ];
    while ( n )
    {
        if ( n->GetNameHash() == hash )
        {
            if ( n->GetName().EqualsI( name ) )
            {
                return n;
            }
        }
        n = n->m_Next;
    }
    return nullptr;
}

// FindNearestNodesInternal
//------------------------------------------------------------------------------
void NodeGraph::FindNearestNodesInternal( const AString & fullPath, Array< NodeWithDistance > & nodes, const uint32_t maxDistance ) const
{
    ASSERT( Thread::IsMainThread() );
    ASSERT( nodes.IsEmpty() );
    ASSERT( false == nodes.IsAtCapacity() );

    if ( fullPath.IsEmpty() )
    {
        return;
    }

    uint32_t worstMinDistance = fullPath.GetLength() + 1;

    for ( size_t i = 0 ; i <= m_NodeMapMaxKey ; i++ )
    {
        for ( Node * node = m_NodeMap[i] ; nullptr != node ; node = node->m_Next )
        {
            const uint32_t d = LevenshteinDistance::DistanceI( fullPath, node->GetName() );

            if ( d > maxDistance )
            {
                continue;
            }

            // skips nodes which don't share any character with fullpath
            if ( fullPath.GetLength() < node->GetName().GetLength() )
            {
                if ( d > node->GetName().GetLength() - fullPath.GetLength() )
                {
                    continue; // completely different <=> d deletions
                }
            }
            else
            {
                if ( d > fullPath.GetLength() - node->GetName().GetLength() )
                {
                    continue; // completely different <=> d deletions
                }
            }

            if ( nodes.IsEmpty() )
            {
                nodes.EmplaceBack( node, d );
                worstMinDistance = nodes.Top().m_Distance;
            }
            else if ( d >= worstMinDistance )
            {
                ASSERT( nodes.IsEmpty() || nodes.Top().m_Distance == worstMinDistance );
                if ( false == nodes.IsAtCapacity() )
                {
                    nodes.EmplaceBack( node, d );
                    worstMinDistance = d;
                }
            }
            else
            {
                ASSERT( nodes.Top().m_Distance > d );
                const size_t count = nodes.GetSize();

                if ( false == nodes.IsAtCapacity() )
                {
                    nodes.EmplaceBack();
                }

                size_t pos = count;
                for ( ; pos > 0 ; pos-- )
                {
                    if ( nodes[pos - 1].m_Distance <= d )
                    {
                        break;
                    }
                    else if (pos < nodes.GetSize() )
                    {
                        nodes[pos] = nodes[pos - 1];
                    }
                }

                ASSERT( pos < count );
                nodes[pos] = NodeWithDistance( node, d );
                worstMinDistance = nodes.Top().m_Distance;
            }
        }
    }
}

// UpdateBuildStatus
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::UpdateBuildStatus( const Node * node,
                                              uint32_t & nodesBuiltTime,
                                              uint32_t & totalNodeTime )
{
    s_BuildPassTag++;
    UpdateBuildStatusRecurse( node, nodesBuiltTime, totalNodeTime );
}

// UpdateBuildStatusRecurse
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::UpdateBuildStatusRecurse( const Node * node,
                                                     uint32_t & nodesBuiltTime,
                                                     uint32_t & totalNodeTime )
{
    // record time for this node
    uint32_t nodeTime = node->GetLastBuildTime();
    totalNodeTime += nodeTime;
    nodesBuiltTime += ( node->GetState() == Node::UP_TO_DATE ) ? nodeTime : 0;

    // record accumulated child time if available
    uint32_t accumulatedProgress = node->GetProgressAccumulator();
    if ( accumulatedProgress > 0 )
    {
        nodesBuiltTime += accumulatedProgress;
        totalNodeTime += accumulatedProgress;
        return;
    }

    // don't recurse the same node multiple times in the same pass
    const uint32_t buildPassTag = s_BuildPassTag;
    if ( node->GetBuildPassTag() == buildPassTag )
    {
        return;
    }
    node->SetBuildPassTag( buildPassTag );

    // calculate time for children

    uint32_t progress = 0;
    uint32_t total = 0;

    UpdateBuildStatusRecurse( node->GetPreBuildDependencies(), progress, total );
    UpdateBuildStatusRecurse( node->GetStaticDependencies(), progress, total );
    UpdateBuildStatusRecurse( node->GetDynamicDependencies(), progress, total );

    nodesBuiltTime += progress;
    totalNodeTime += total;

    // if node is building, then progress of children cannot change
    // and we can store it in the accumulator
    if ( node->GetState() >= Node::BUILDING )
    {
        node->SetProgressAccumulator(total);
    }
}

// UpdateBuildStatusRecurse
//------------------------------------------------------------------------------
/*static*/ void NodeGraph::UpdateBuildStatusRecurse( const Dependencies & dependencies,
                                                     uint32_t & nodesBuiltTime,
                                                     uint32_t & totalNodeTime )
{
    for ( const Dependency & dep : dependencies)
    {
        UpdateBuildStatusRecurse( dep.GetNode(), nodesBuiltTime, totalNodeTime );
    }
}

// CheckForCyclicDependencies
//------------------------------------------------------------------------------
/*static*/ bool NodeGraph::CheckForCyclicDependencies( const Node * node )
{
    //
    // Some cyclic dependency problems can only be detected at build time.
    // This check can be non-trivially expensive with a large dependency graph
    // so we want to avoid the check unless absolutely necessary.
    // Since a cyclic dependency would cause the build to get stuck, we can
    // run the check only in cases where there are no jobs available, no jobs
    // in flight and no jobs about to be added from the last sweep.
    //
    // Some of these things depend on timing, so this check could conceivably run
    // when not stuck, but it will never falsely detect a cyclic dependency.
    //

    // Early out if the root node is being processed
    if ( node->GetState() >= Node::State::BUILDING )
    {
        return false;
    }

    // Early out if we've discovered new jobs to queue in the last graph sweep
    if ( JobQueue::Get().HasJobsToFlush() )
    {
        return false;
    }

    // Early out if there are active jobs in progress
    uint32_t numJobs = 0;
    uint32_t numJobsActive = 0;
    uint32_t numJobsDist = 0;
    uint32_t numJobsDistActive = 0;
    JobQueue::Get().GetJobStats( numJobs, numJobsActive, numJobsDist, numJobsDistActive );
    if ( ( numJobs > 0 ) ||
         ( numJobsActive > 0 ) ||
         ( numJobsDist > 0 ) ||
         ( numJobsDistActive > 0 ) )
    {
        return false;
    }

    // Early out if the JobQueue has completed jobs that haven't been processed yet
    if ( JobQueue::Get().HasPendingCompletedJobs() )
    {
        return false;
    }

    PROFILE_FUNCTION;

    s_BuildPassTag++;
    StackArray< const Node * > dependencyStack;
    return CheckForCyclicDependenciesRecurse( node, dependencyStack );
}

// CheckForCyclicDependenciesRecurse
//------------------------------------------------------------------------------
/*static*/ bool NodeGraph::CheckForCyclicDependenciesRecurse( const Node * node, Array< const Node * > & dependencyStack )
{
    // If dependencies are satisfied, there can't be any circular dependencies
    // below this node
    if ( node->GetState() >= Node::State::BUILDING )
    {
        return false;
    }

    // Check if we've recursed into ourselves
    if ( dependencyStack.Find( node ) )
    {
        AStackString<> buffer( "Error: Cyclic dependency detected. Dependency chain:\n" );
        for ( const Node * nodeInStack : dependencyStack )
        {
            // Exclude the proxy node that can sometimes appear at the root
            if ( nodeInStack->GetType() != Node::PROXY_NODE )
            {
                buffer.AppendFormat( " - %s%s\n", nodeInStack->GetName().Get(), ( nodeInStack == node ) ? " <--- HERE" : "" );
            }
        }
        FLOG_ERROR( "%s - %s <--- HERE\n", buffer.Get(), node->GetName().Get() );
        return true;
    }

    // Don't check this node again in the same sweep
    const uint32_t buildPassTag = s_BuildPassTag;
    if ( node->GetBuildPassTag() == buildPassTag )
    {
        return false;
    }
    node->SetBuildPassTag( buildPassTag );

    // Add self to stack
    dependencyStack.Append( node );

    // Recurse
    if ( CheckForCyclicDependenciesRecurse( node->GetPreBuildDependencies(), dependencyStack ) ||
         CheckForCyclicDependenciesRecurse( node->GetStaticDependencies(), dependencyStack ) ||
         CheckForCyclicDependenciesRecurse( node->GetDynamicDependencies(), dependencyStack ) )
    {
        return true;
    }

    // Remove self from stack
    dependencyStack.Pop();

    return false;
}

// UpdateBuildStatusRecurse
//------------------------------------------------------------------------------
/*static*/ bool NodeGraph::CheckForCyclicDependenciesRecurse( const Dependencies & dependencies,
                                                                Array< const Node * > & dependencyStack )
{
    for ( const Dependency & dep : dependencies )
    {
        if ( CheckForCyclicDependenciesRecurse( dep.GetNode(), dependencyStack ) )
        {
            return true;
        }
    }
    return false;
}

// ReadHeaderAndUsedFiles
//------------------------------------------------------------------------------
bool NodeGraph::ReadHeaderAndUsedFiles( ConstMemoryStream & nodeGraphStream, const char* nodeGraphDBFile, Array< UsedFile > & files, bool & compatibleDB, bool & movedDB ) const
{
    // Assume good DB by default (cases below will change flags if needed)
    compatibleDB = true;
    movedDB = false;

    // check for a valid header
    NodeGraphHeader ngh;
    if ( ( nodeGraphStream.Read( &ngh, sizeof( ngh ) ) != sizeof( ngh ) ) ||
         ( ngh.IsValid() == false ) )
    {
        return false;
    }

    // check if version is loadable
    if ( ngh.IsCompatibleVersion() == false )
    {
        compatibleDB = false;
        return true;
    }

    // Check contents of stream is valid
    {
        const uint64_t tell = nodeGraphStream.Tell();
        ASSERT( tell == sizeof( NodeGraphHeader ) ); // Stream should be after header
        const char* data = ( static_cast<const char*>( nodeGraphStream.GetData() ) + tell );
        const size_t remainingSize = ( nodeGraphStream.GetSize() - tell );
        const uint64_t hash = xxHash3::Calc64( data, remainingSize );
        if ( hash != ngh.GetContentHash() )
        {
            return false; // DB is corrupt
        }
    }

    // Read location where .fdb was originally saved
    AStackString<> originalNodeGraphDBFile;
    if ( !nodeGraphStream.Read( originalNodeGraphDBFile ) )
    {
        return false;
    }
    AStackString<> nodeGraphDBFileClean( nodeGraphDBFile );
    NodeGraph::CleanPath( nodeGraphDBFileClean );
    if ( PathUtils::ArePathsEqual( originalNodeGraphDBFile, nodeGraphDBFileClean ) == false )
    {
        movedDB = true;
        FLOG_WARN( "Database has been moved (originally at '%s', now at '%s').", originalNodeGraphDBFile.Get(), nodeGraphDBFileClean.Get() );
        if ( FBuild::Get().GetOptions().m_ContinueAfterDBMove )
        {
            // Allow build to continue (will be a clean build)
            compatibleDB = false;
            return true;
        }
        return false;
    }

    uint32_t numFiles = 0;
    if ( !nodeGraphStream.Read( numFiles ) )
    {
        return false;
    }

    for ( uint32_t i=0; i<numFiles; ++i )
    {
        uint32_t fileNameLen( 0 );
        if ( !nodeGraphStream.Read( fileNameLen ) )
        {
            return false;
        }
        AStackString<> fileName;
        fileName.SetLength( fileNameLen ); // handles null terminate
        if ( nodeGraphStream.Read( fileName.Get(), fileNameLen ) != fileNameLen )
        {
            return false;
        }
        uint64_t timeStamp;
        if ( !nodeGraphStream.Read( timeStamp ) )
        {
            return false;
        }
        uint64_t dataHash;
        if ( !nodeGraphStream.Read( dataHash ) )
        {
            return false;
        }

        files.EmplaceBack( fileName, timeStamp, dataHash );
    }

    return true;
}

// GetLibEnvVarHash
//------------------------------------------------------------------------------
uint32_t NodeGraph::GetLibEnvVarHash() const
{
    // ok for LIB var to be missing, we'll hash the empty string
    AStackString<> libVar;
    FBuild::Get().GetLibEnvVar( libVar );
    return xxHash::Calc32( libVar );
}

// IsCleanPath
//------------------------------------------------------------------------------
#if defined( ASSERTS_ENABLED )
    /*static*/ bool NodeGraph::IsCleanPath( const AString & path )
    {
        AStackString< 1024 > clean;
        CleanPath( path, clean );
        return ( path == clean );
    }
#endif

// Migrate
//------------------------------------------------------------------------------
void NodeGraph::Migrate( const NodeGraph & oldNodeGraph )
{
    PROFILE_FUNCTION;

    s_BuildPassTag++;

    // NOTE: m_AllNodes can change during recursion, so we must take care to
    // iterate by index (array might move due to resizing). Any newly added
    // nodes will already be traversed so we only need to check the original
    // range here
    const size_t numNodes = m_AllNodes.GetSize();
    for ( size_t i=0; i<numNodes; ++i )
    {
        Node & newNode = *m_AllNodes[ i ];
        MigrateNode( oldNodeGraph, newNode, nullptr );
    }
}

// MigrateNode
//------------------------------------------------------------------------------
void NodeGraph::MigrateNode( const NodeGraph & oldNodeGraph, Node & newNode, const Node * oldNodeHint )
{
    // Prevent visiting the same node twice
    if ( newNode.GetBuildPassTag() == s_BuildPassTag )
    {
        return;
    }
    newNode.SetBuildPassTag( s_BuildPassTag );

    // FileNodes (inputs to the build) build every time so don't need migration
    if ( newNode.GetType() == Node::FILE_NODE )
    {
        return;
    }

    // Migrate children before parents
    for ( const Dependency & dep : newNode.m_PreBuildDependencies )
    {
        MigrateNode( oldNodeGraph, *dep.GetNode(), nullptr );
    }
    for ( const Dependency & dep : newNode.m_StaticDependencies )
    {
        MigrateNode( oldNodeGraph, *dep.GetNode(), nullptr );
    }

    // Get the matching node in the old DB
    const Node * oldNode;
    if ( oldNodeHint )
    {
        // Use the node passed in (if calling code already knows it saves us a lookup)
        oldNode = oldNodeHint;
        ASSERT( oldNode == oldNodeGraph.FindNodeInternal( newNode.GetName(), newNode.GetNameHash() ) );
    }
    else
    {
        // Find the old node
        oldNode = oldNodeGraph.FindNodeInternal( newNode.GetName(), newNode.GetNameHash() );
        if ( oldNode == nullptr )
        {
            // The newNode has no equivalent in the old DB.
            // This is either:
            //  - a brand new target defined in the bff
            //  - an existing target that has never been built
            return;
        }
    }

    // Has the node changed type?
    const ReflectionInfo * oldNodeRI = oldNode->GetReflectionInfoV();
    const ReflectionInfo * newNodeRI = newNode.GetReflectionInfoV();
    if ( oldNodeRI != newNodeRI )
    {
        // The newNode has changed type (the build rule has changed)
        return;
    }

    // Have the properties on the node changed?
    if ( AreNodesTheSame( oldNode, &newNode, newNodeRI ) == false )
    {
        // Properties have changed. We need to rebuild with the new
        // properties.
        return;
    }

    // PreBuildDependencies
    if ( DoDependenciesMatch( oldNode->m_PreBuildDependencies, newNode.m_PreBuildDependencies ) == false )
    {
        return;
    }

    // StaticDependencies
    if ( DoDependenciesMatch( oldNode->m_StaticDependencies, newNode.m_StaticDependencies ) == false )
    {
        return;
    }

    // Migrate static Dependencies
    // - since everything matches, we only need to migrate the stamps
    for ( Dependency & dep : newNode.m_StaticDependencies )
    {
        const size_t index = newNode.m_StaticDependencies.GetIndexOf( &dep );
        const Dependency & oldDep = oldNode->m_StaticDependencies[ index ];
        dep.Stamp( oldDep.GetNodeStamp() );
    }

    // Migrate Dynamic Dependencies
    {
        // New node should have no dynamic dependencies
        ASSERT( newNode.m_DynamicDependencies.GetSize() == 0 );
        const Dependencies & oldDeps = oldNode->m_DynamicDependencies;
        Dependencies newDeps( oldDeps.GetSize() );
        for ( const Dependency & oldDep : oldDeps )
        {
            // See if the dependency already exists in the new DB
            const Node * oldDepNode = oldDep.GetNode();
            Node * newDepNode = FindNodeInternal( oldDepNode->GetName(), oldDepNode->GetNameHash() );

            // If the dependency exists, but has changed type, then dependencies
            // cannot be transferred.
            if ( newDepNode && ( newDepNode->GetType() != oldDepNode->GetType() ) )
            {
                return; // No point trying the remaining deps as node will need rebuilding anyway
            }
            if ( newDepNode )
            {
                newDeps.Add( newDepNode, oldDep.GetNodeStamp(), oldDep.IsWeak() );
            }
            else
            {
                // Create the dependency
                newDepNode = CreateNode( oldDepNode->GetType(),
                                         Move( AString( oldDepNode->GetName() ) ),
                                         oldDepNode->GetNameHash() );
                ASSERT( newDepNode );
                newDeps.Add( newDepNode, oldDep.GetNodeStamp(), oldDep.IsWeak() );

                // Early out for FileNode (no properties and doesn't need Initialization)
                if ( oldDepNode->GetType() == Node::FILE_NODE )
                {
                    continue;
                }

                // Transfer all the properties
                MigrateProperties( (const void *)oldDepNode, (void *)newDepNode, newDepNode->GetReflectionInfoV() );

                // Initialize the new node
                const BFFToken * token = nullptr;
                VERIFY( newDepNode->Initialize( *this, token, nullptr ) );

                // Continue recursing
                MigrateNode( oldNodeGraph, *newDepNode, oldDepNode );
            }
        }
        if ( newDeps.IsEmpty() == false )
        {
            newNode.m_DynamicDependencies.Add( newDeps );
        }
    }

    // If we get here, then everything about the node is unchanged from the
    // old DB to the new DB, so we can transfer the node's internal state. This
    // will prevent the node rebuilding unnecessarily.
    newNode.Migrate( *oldNode );
}

// MigrateProperties
//------------------------------------------------------------------------------
void NodeGraph::MigrateProperties( const void * oldBase, void * newBase, const ReflectionInfo * ri )
{
    // Are all properties the same?
    do
    {
        const ReflectionIter end = ri->End();
        for ( ReflectionIter it = ri->Begin(); it != end; ++it )
        {
            MigrateProperty( oldBase, newBase, *it );
        }

        // Traverse into parent class (if there is one)
        ri = ri->GetSuperClass();
    }
    while( ri );
}

// MigrateProperty
//------------------------------------------------------------------------------
void NodeGraph::MigrateProperty( const void * oldBase, void * newBase, const ReflectedProperty & property )
{
    switch ( property.GetType() )
    {
        case PropertyType::PT_ASTRING:
        {
            if ( property.IsArray() )
            {
                const Array< AString > * stringsOld = property.GetPtrToArray<AString>( oldBase );
                Array< AString > * stringsNew = property.GetPtrToArray<AString>( newBase );
                *stringsNew = *stringsOld;
            }
            else
            {
                const AString * stringOld = property.GetPtrToProperty<AString>( oldBase );
                AString * stringNew = property.GetPtrToProperty<AString>( newBase );
                *stringNew = *stringOld;
            }
            break;
        }
        case PT_UINT8:
        {
            ASSERT( property.IsArray() == false );

            const uint8_t * u8Old = property.GetPtrToProperty<uint8_t>( oldBase );
            uint8_t * u8New = property.GetPtrToProperty<uint8_t>( newBase );
            *u8New = *u8Old;
            break;
        }
        case PT_INT32:
        {
            ASSERT( property.IsArray() == false );
            const int32_t * i32Old = property.GetPtrToProperty<int32_t>( oldBase );
            int32_t * i32New = property.GetPtrToProperty<int32_t>( newBase );
            *i32New = *i32Old;
            break;
        }
        case PT_UINT32:
        {
            ASSERT( property.IsArray() == false );
            const uint32_t * u32Old = property.GetPtrToProperty<uint32_t>( oldBase );
            uint32_t * u32New = property.GetPtrToProperty<uint32_t>( newBase );
            *u32New = *u32Old;
            break;
        }
        case PT_UINT64:
        {
            ASSERT( property.IsArray() == false );
            const uint64_t * u64Old = property.GetPtrToProperty<uint64_t>( oldBase );
            uint64_t * u64New = property.GetPtrToProperty<uint64_t>( newBase );
            *u64New = *u64Old;
            break;
        }
        case PT_BOOL:
        {
            ASSERT( property.IsArray() == false );
            const bool * bOld = property.GetPtrToProperty<bool>( oldBase );
            bool * bNew = property.GetPtrToProperty<bool>( newBase );
            *bNew = *bOld;
            break;
        }
        case PT_STRUCT:
        {
            const ReflectedPropertyStruct & propertyStruct = static_cast<const ReflectedPropertyStruct &>( property );
            if ( property.IsArray() )
            {
                const uint32_t numElements = (uint32_t)propertyStruct.GetArraySize( oldBase );
                propertyStruct.ResizeArrayOfStruct( newBase, numElements );
                for ( uint32_t i=0; i<numElements; ++i )
                {
                    MigrateProperties( propertyStruct.GetStructInArray( oldBase, i ), propertyStruct.GetStructInArray( newBase, i ), propertyStruct.GetStructReflectionInfo() );
                }
            }
            else
            {
                MigrateProperties( propertyStruct.GetStructBase( oldBase ), propertyStruct.GetStructBase( newBase ), propertyStruct.GetStructReflectionInfo() );
            }
            break;
        }
        default: ASSERT( false ); // Unhandled
    }
}

// AreNodesTheSame
//------------------------------------------------------------------------------
/*static*/ bool NodeGraph::AreNodesTheSame( const void * baseA, const void * baseB, const ReflectionInfo * ri )
{
    // Are all properties the same?
    do
    {
        const ReflectionIter end = ri->End();
        for ( ReflectionIter it = ri->Begin(); it != end; ++it )
        {
            // Is this property the same?
            if ( AreNodesTheSame( baseA, baseB, *it ) == false )
            {
                return false;
            }
        }

        // Traverse into parent class (if there is one)
        ri = ri->GetSuperClass();
    }
    while( ri );

    return true;
}

// AreNodesTheSame
//------------------------------------------------------------------------------
/*static*/ bool NodeGraph::AreNodesTheSame( const void * baseA, const void * baseB, const ReflectedProperty & property )
{
    if ( property.HasMetaData< Meta_IgnoreForComparison >() )
    {
        return true;
    }

    switch ( property.GetType() )
    {
        case PropertyType::PT_ASTRING:
        {
            if ( property.IsArray() )
            {
                const Array< AString > * stringsA = property.GetPtrToArray<AString>( baseA );
                const Array< AString > * stringsB = property.GetPtrToArray<AString>( baseB );
                if ( stringsA->GetSize() != stringsB->GetSize() )
                {
                    return false;
                }
                const size_t numStrings = stringsA->GetSize();
                for ( size_t i=0; i<numStrings; ++i )
                {
                    if ( (*stringsA)[ i ] != (*stringsB)[ i ] )
                    {
                        return false;
                    }
                }
            }
            else
            {
                const AString * stringA = property.GetPtrToProperty<AString>( baseA );
                const AString * stringB = property.GetPtrToProperty<AString>( baseB );
                if ( *stringA != *stringB )
                {
                    return false;
                }
            }
            break;
        }
        case PT_UINT8:
        {
            ASSERT( property.IsArray() == false );

            const uint8_t * u8A = property.GetPtrToProperty<uint8_t>( baseA );
            const uint8_t * u8B = property.GetPtrToProperty<uint8_t>( baseB );
            if ( *u8A != *u8B )
            {
                return false;
            }
            break;
        }
        case PT_INT32:
        {
            ASSERT( property.IsArray() == false );
            const int32_t * i32A = property.GetPtrToProperty<int32_t>( baseA );
            const int32_t * i32B = property.GetPtrToProperty<int32_t>( baseB );
            if ( *i32A != *i32B )
            {
                return false;
            }
            break;
        }
        case PT_UINT32:
        {
            ASSERT( property.IsArray() == false );

            const uint32_t * u32A = property.GetPtrToProperty<uint32_t>( baseA );
            const uint32_t * u32B = property.GetPtrToProperty<uint32_t>( baseB );
            if ( *u32A != *u32B )
            {
                return false;
            }
            break;
        }
        case PT_UINT64:
        {
            ASSERT( property.IsArray() == false );

            const uint64_t * u64A = property.GetPtrToProperty<uint64_t>( baseA );
            const uint64_t * u64B = property.GetPtrToProperty<uint64_t>( baseB );
            if ( *u64A != *u64B )
            {
                return false;
            }
            break;
        }
        case PT_BOOL:
        {
            ASSERT( property.IsArray() == false );

            const bool * bA = property.GetPtrToProperty<bool>( baseA );
            const bool * bB = property.GetPtrToProperty<bool>( baseB );
            if ( *bA != *bB )
            {
                return false;
            }
            break;
        }
        case PT_STRUCT:
        {
            const ReflectedPropertyStruct & propertyStruct = static_cast<const ReflectedPropertyStruct &>( property );
            if ( property.IsArray() )
            {
                const uint32_t numElementsA = (uint32_t)propertyStruct.GetArraySize( baseA );
                const uint32_t numElementsB = (uint32_t)propertyStruct.GetArraySize( baseB );
                if ( numElementsA != numElementsB )
                {
                    return false;
                }
                for ( uint32_t i=0; i<numElementsA; ++i )
                {
                    if ( AreNodesTheSame( propertyStruct.GetStructInArray( baseA, i ), propertyStruct.GetStructInArray( baseB, i ), propertyStruct.GetStructReflectionInfo() ) == false )
                    {
                        return false;
                    }
                }
            }
            else
            {
                if ( AreNodesTheSame( propertyStruct.GetStructBase( baseA ), propertyStruct.GetStructBase( baseB ), propertyStruct.GetStructReflectionInfo() ) == false )
                {
                    return false;
                }
            }
            break;
        }
        default: ASSERT( false ); // Unhandled
    }

    return true;
}

// DoDependenciesMatch
//------------------------------------------------------------------------------
bool NodeGraph::DoDependenciesMatch( const Dependencies & depsA, const Dependencies & depsB )
{
    if ( depsA.GetSize() != depsB.GetSize() )
    {
        return false;
    }

    const size_t numDeps = depsA.GetSize();
    for ( size_t i = 0; i<numDeps; ++i )
    {
        const Node * nodeA = depsA[ i ].GetNode();
        const Node * nodeB = depsB[ i ].GetNode();
        if ( nodeA->GetType() != nodeB->GetType() )
        {
            return false;
        }
        if ( nodeA->GetStamp() != nodeB->GetStamp() )
        {
            return false;
        }
        if ( nodeA->GetName() != nodeB->GetName() )
        {
            return false;
        }
    }

    return true;
}

//------------------------------------------------------------------------------
