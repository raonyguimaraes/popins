#ifndef POPINS_CONTIGMAP_H_
#define POPINS_CONTIGMAP_H_

#include <sstream>

#include <seqan/file.h>
#include <seqan/sequence.h>
#include <seqan/bam_io.h>

#include "popins_clp.h"
#include "popins_crop_unmapped.h"

using namespace seqan;

// ==========================================================================
// Function write_fastq()
// ==========================================================================

bool
write_fastq(CharString & fastqFirst,
            CharString & fastqSecond,
            CharString & fastqSingle,
            CharString & unmappedBam)
{
    typedef std::map<CharString, Pair<CharString> > TFastqMap;
    
    // Create maps for fastq records (first read in pair and second read in pair).
    TFastqMap firstReads, secondReads;

    // Open bam file.
    BamStream inStream(toCString(unmappedBam), BamStream::READ);
    if (!isGood(inStream)) 
    {
        std::cerr << "ERROR while opening input bam file " << unmappedBam << std::endl;
        return 1;
    }
    
    // Iterate over bam file and append fastq records.
    BamAlignmentRecord record;
    while(!atEnd(inStream))
    {
        readRecord(record, inStream);
        if (hasFlagUnmapped(record))
            appendFastqRecord(firstReads, secondReads, record);
    }

    // Write the fastq files.
    if (writeFastq(fastqFirst, fastqSecond, fastqSingle, firstReads, secondReads) != 0) return 1;

    return 0;
}

// ==========================================================================
// Function fill_sequences()
// ==========================================================================

bool
fill_sequences(BamStream & outStream, BamStream & inStream)
{
    typedef Position<Dna5String>::Type TPos;

    BamAlignmentRecord firstRecord, nextRecord;
    
    // read the first record from bam file
//    if (!atEnd(inStream))
//    {
//        readRecord(nextRecord, inStream);
//        writeRecord(outStream, nextRecord);
//    }
        
    while (!atEnd(inStream))
    {
        readRecord(nextRecord, inStream);
        
        if (firstRecord.qName != nextRecord.qName || hasFlagFirst(firstRecord) != hasFlagFirst(nextRecord))
        {
            // update first record
            firstRecord = nextRecord;
            if (length(firstRecord.seq) == 0 || length(firstRecord.qual) == 0)
            {
                std::cerr << "ERROR: First record of read " << firstRecord.qName << " has no sequence." << std::endl;
                return 1;
            }
        }
        else
        {
            // fill sequence field and quality string
            if (length(nextRecord.seq) == 0 || length(nextRecord.qual) == 0)
            {
                TPos last = length(nextRecord.cigar)-1;
                if (nextRecord.cigar[0].operation == 'H' || nextRecord.cigar[last].operation == 'H')
                {
                    TPos begin = 0;
                    if (nextRecord.cigar[0].operation == 'H')
                        begin = nextRecord.cigar[0].count;

                    TPos end = length(firstRecord.seq);
                    if (nextRecord.cigar[last].operation == 'H')
                        end -= nextRecord.cigar[last].count;

                    nextRecord.seq = infix(firstRecord.seq, begin, end);
                    nextRecord.qual = infix(firstRecord.qual, begin, end);
                }
                else
                {
                    nextRecord.seq = firstRecord.seq;
                    nextRecord.qual = firstRecord.qual;
                }
            }
        }
        
        writeRecord(outStream, nextRecord);
    }
    return 0;
}


// ==========================================================================
// Function popins_contigmap()
// ==========================================================================

int popins_contigmap(int argc, char const ** argv)
{
    // Parse the command line to get option values.
    ContigMapOptions options;
    if (parseCommandLine(options, argc, argv) != 0)
        return 1;

    CharString fastqFirst = getFileName(options.workingDirectory, "paired.1.fastq");
    CharString fastqSecond = getFileName(options.workingDirectory, "paired.2.fastq");
    CharString fastqSingle = getFileName(options.workingDirectory, "single.fastq");
    CharString nonRefBam = getFileName(options.workingDirectory, "non_ref.bam");

    if (!exists(fastqFirst) || !exists(fastqSecond) || !exists(fastqSingle) || !exists(nonRefBam))
    {
        std::cerr << "ERROR: Could not find all input files ";
        std::cerr << fastqFirst << ", " << fastqSecond << ", " << fastqSingle << ", or " << nonRefBam << std::endl;
        return 1;
    }

    CharString mappedSam = getFileName(options.workingDirectory, "contig_mapped_unsorted.sam");
    CharString mappedBamUnsorted = getFileName(options.workingDirectory, "contig_mapped_unsorted.bam");
    CharString mappedBam = getFileName(options.workingDirectory, "contig_mapped.bam");
    CharString mergedBam = getFileName(options.workingDirectory, "merged.bam");

    // Remapping to contigs with bwa.
    std::cerr << "[" << time(0) << "] Mapping reads to contigs using " << BWA << std::endl;
    std::stringstream cmd;
    cmd.str("");
    cmd << BWA << " mem -a " << options.contigFile << " " << fastqFirst << " " << fastqSecond << " > " << mappedSam;
    if (system(cmd.str().c_str()) != 0)
    {
        std::cerr << "ERROR while running bwa on " << fastqFirst << " and " << fastqSecond << std::endl;
        return 1;
    }
    //remove(toCString(fastqFirst));
    //remove(toCString(fastqSecond));

    cmd.str("");
    cmd << BWA << " mem -a " << options.contigFile << " " << fastqSingle << " | awk '$1 !~ /@/' >> " << mappedSam;
    if (system(cmd.str().c_str()) != 0)
    {
        std::cerr << "ERROR while running bwa on " << fastqSingle << std::endl;
        return 1;
    }
    //remove(toCString(fastqSingle));

    // Fill in sequences in bwa output.
    std::cerr << "[" << time(0) << "] Filling in sequences of secondary records in bwa output" << std::endl;
    BamStream samStream(toCString(mappedSam));
    if (!isGood(samStream))
    {
        std::cerr << "ERROR: Could bwa output file " << mappedSam << "" << std::endl;
        return 1;
    }
    BamStream bamStream(toCString(mappedBamUnsorted), BamStream::WRITE);
    if (!isGood(bamStream))
    {
        std::cerr << "ERROR: Could not open output file " << mappedBamUnsorted << "" << std::endl;
        return 1;
    }
    bamStream.header = samStream.header;
    fill_sequences(bamStream, samStream);
    close(bamStream);
    remove(toCString(mappedSam));

    // Sort <WD>/contig_mapped.bam by read name
    std::cerr << "[" << time(0) << "] " << "Sorting " << mappedBamUnsorted << " by read name using " << SAMTOOLS << std::endl;
    cmd.str("");
    cmd << SAMTOOLS << " sort -n " << mappedBamUnsorted << " " << options.workingDirectory << "/contig_mapped";
    if (system(cmd.str().c_str()) != 0)
    {
        std::cerr << "ERROR while sorting " << mappedBamUnsorted << " by read name using " << SAMTOOLS << std::endl;
        return 1;
    }
    remove(toCString(mappedBamUnsorted));
    
    merge_and_set_mate(mergedBam, nonRefBam, mappedBam);
    remove(toCString(mappedBam));
    remove(toCString(nonRefBam));

    // Sort <WD>/merged.bam by beginPos, output is <WD>/non_ref.bam.
    std::cerr << "[" << time(0) << "] " << "Sorting " << mergedBam << " using " << SAMTOOLS << std::endl;
    cmd.str("");
    cmd << SAMTOOLS << " sort " << mergedBam << " " << options.workingDirectory << "/non_ref";
    if (system(cmd.str().c_str()) != 0)
    {
        std::cerr << "ERROR while sorting " << mergedBam << " by beginPos using " << SAMTOOLS << std::endl;
        return 1;
    }
    
    remove(toCString(mergedBam));
    
    // Index <WD>/non_ref.bam.
    std::cerr << "[" << time(0) << "] " << "Indexing " << nonRefBam << " by beginPos using " << SAMTOOLS << std::endl;
    cmd.str("");
    cmd << SAMTOOLS << " index " << nonRefBam;
    if (system(cmd.str().c_str()) != 0)
    {
        std::cerr << "ERROR while indexing " << nonRefBam << " using " << SAMTOOLS << std::endl;
        return 1;
    }
    
    return 0;
}

#endif // #ifndef POPINS_CONTIGMAP_H_
