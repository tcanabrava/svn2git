/*
 *  Copyright (C) 2007  Thiago Macieira <thiago@kde.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QDebug>

#include <limits.h>
#include <stdio.h>

#include "CommandLineParser.h"
#include "ruleparser.h"
#include "repository.h"
#include "svn.h"

QHash<QByteArray, QByteArray> loadIdentityMapFile(const QString &fileName)
{
    QHash<QByteArray, QByteArray> result;
    if (fileName.isEmpty())
        return result;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "Could not open file %s: %s",
                qPrintable(fileName), qPrintable(file.errorString()));
        return result;
    }

    while (!file.atEnd()) {
        QByteArray line = file.readLine();
        int comment_pos = line.indexOf('#');
        if (comment_pos != -1)
            line.truncate(comment_pos);
        line = line.trimmed();
        int space = line.indexOf(' ');
        if (space == -1)
            continue;           // invalid line

        // Support git-svn author files, too
        // - svn2git native:  loginname Joe User <user@example.com>
        // - git-svn:         loginname = Joe User <user@example.com>
        int rightspace = space;
        if (line.indexOf(" = ") == space)
            rightspace += 2;

        QByteArray realname = line.mid(rightspace).trimmed();
        line.truncate(space);
        result.insert(line, realname);
    };
    file.close();

    return result;
}

QSet<int> loadRevisionsFile( const QString &fileName )
{
    QSet<int> revisions;
    if(fileName.isEmpty())
        return revisions;

    QFile file(fileName);
    if( !file.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "Could not open file %s: %s", qPrintable(fileName), qPrintable(file.errorString()));
        return revisions;
    }

    while(!file.atEnd()) {
        QByteArray line = file.readLine();
        bool ok;
        line = line.trimmed();
        int rev = line.toInt(&ok);
        if(!ok) {
            fprintf(stderr, "Unable to convert %s to int, skipping revision.", qPrintable(QString(line)));
        } else {
            revisions.insert(rev);
        }
    }
    file.close();
    return revisions;
}

static const CommandLineOption options[] = {
    {"--identity-map FILENAME", "provide map between svn username and email"},
    {"--revisions-file FILENAME", "provide a file with revision number that should be processed"},
    {"--rules FILENAME[,FILENAME]", "the rules file(s) that determines what goes where"},
    {"--add-metadata", "if passed, each git commit will have svn commit info"},
    {"--resume-from revision", "start importing at svn revision number"},
    {"--max-rev revision", "stop importing at svn revision number"},
    {"--dry-run", "don't actually write anything"},
    {"--debug-rules", "print what rule is being used for each file"},
    {"--commit-interval NUMBER", "if passed the cache will be flushed to git every NUMBER of commits"},
    {"--stats", "after a run print some statistics about the rules"},
    {"-h, --help", "show help"},
    {"-v, --version", "show version"},
    CommandLineLastOption
};

int main(int argc, char **argv)
{
    CommandLineParser::init(argc, argv);
    CommandLineParser::addOptionDefinitions(options);
    Stats::init();
    CommandLineParser *args = CommandLineParser::instance();
    if (args->contains(QLatin1String("help")) || args->arguments().count() != 1) {
        args->usage(QString(), "[Path to subversion repo]");
        return 0;
    }
    if (args->undefinedOptions().count()) {
        QTextStream out(stderr);
        out << "svn-all-fast-export failed: ";
        bool first = true;
        foreach (QString option, args->undefinedOptions()) {
            if (!first)
                out << "          : ";
            out << "unrecognized option or missing argument for; `" << option << "'" << endl;
            first = false;
        }
        return 10;
    }
    if (!args->contains("rules")) {
        QTextStream out(stderr);
        out << "svn-all-fast-export failed: please specify the rules using the 'rules' argument\n";
        return 11;
    }
    if (!args->contains("identity-map")) {
        QTextStream out(stderr);
        out << "WARNING; no identity-map specified, all commits will be without email address\n\n";
    }

    QCoreApplication app(argc, argv);
    // Load the configuration
    RulesList rulesList(args->optionArgument(QLatin1String("rules")));
    rulesList.load();

    int resume_from = args->optionArgument(QLatin1String("resume-from")).toInt();
    int max_rev = args->optionArgument(QLatin1String("max-rev")).toInt();

    // create the repository list
    QHash<QString, Repository *> repositories;

    int cutoff = resume_from ? resume_from : INT_MAX;
 retry:
    int min_rev = 1;
    foreach (Rules::Repository rule, rulesList.allRepositories()) {
        Repository *repo = makeRepository(rule, repositories);
        if (!repo)
            return EXIT_FAILURE;
        repositories.insert(rule.name, repo);

        int repo_next = repo->setupIncremental(cutoff);

        /*
  * cutoff < resume_from => error exit eventually
  * repo_next == cutoff => probably truncated log
  */
        if (cutoff < resume_from && repo_next == cutoff)
            /*
      * Restore the log file so we fail the next time
      * svn2git is invoked with the same arguments
      */
            repo->restoreLog();

        if (cutoff < min_rev)
            /*
      * We've rewound before the last revision of some
      * repository that we've already seen.  Start over
      * from the beginning.  (since cutoff is decreasing,
      * we're sure we'll make forward progress eventually)
      */
            goto retry;

        if (min_rev < repo_next)
            min_rev = repo_next;
    }

    if (cutoff < resume_from) {
        qCritical() << "Cannot resume from" << resume_from
                    << "as there are errors in revision" << cutoff;
        return EXIT_FAILURE;
    }

    if (min_rev < resume_from)
        qDebug() << "skipping revisions" << min_rev << "to" << resume_from - 1 << "as requested";

    if (resume_from)
        min_rev = resume_from;

    Svn::initialize();
    Svn svn(args->arguments().first());
    svn.setMatchRules(rulesList.allMatchRules());
    svn.setRepositories(repositories);
    svn.setIdentityMap(loadIdentityMapFile(args->optionArgument("identity-map")));

    if (max_rev < 1)
        max_rev = svn.youngestRevision();

    bool errors = false;
    QSet<int> revisions = loadRevisionsFile(args->optionArgument(QLatin1String("revisions-file")));
    const bool filerRevisions = !revisions.isEmpty();
    for (int i = min_rev; i <= max_rev; ++i) {
        if(filerRevisions) {
            if( !revisions.contains(i) ) {
                printf(".");
                continue;
            } else {
                printf("\n");
            }
        }
        if (!svn.exportRevision(i)) {
            errors = true;
            break;
        }
    }

    foreach (Repository *repo, repositories) {
        repo->finalizeTags();
        delete repo;
    }
    Stats::instance()->printStats();
    return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
