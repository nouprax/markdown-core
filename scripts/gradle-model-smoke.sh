#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT_DIR/scripts/lib/discover-toolchain.sh"
cd "$ROOT_DIR"

# Download and validate the pinned distribution before locating its Tooling API.
scripts/gradle.sh --version >/dev/null

gradle_version=$(sed -n \
    's#^distributionUrl=.*gradle-\([0-9][0-9.]*\)-bin\.zip$#\1#p' \
    gradle/wrapper/gradle-wrapper.properties)
if [ -z "$gradle_version" ]; then
    echo "Could not derive the Gradle version from gradle-wrapper.properties" >&2
    exit 1
fi

GRADLE_USER_HOME=${GRADLE_USER_HOME:-$HOME/.gradle}
tooling_jar=$(find "$GRADLE_USER_HOME/wrapper/dists" \
    -path "*/gradle-$gradle_version/lib/gradle-tooling-api-$gradle_version.jar" \
    -print | head -n 1)

if [ -z "$tooling_jar" ]; then
    echo "Gradle Tooling API $gradle_version was not found under $GRADLE_USER_HOME/wrapper/dists" >&2
    exit 1
fi

gradle_home=$(dirname "$(dirname "$tooling_jar")")
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

cat >"$temp_dir/GradleModelSmoke.java" <<'JAVA'
import java.io.File;
import org.gradle.tooling.GradleConnector;
import org.gradle.tooling.ProjectConnection;
import org.gradle.tooling.model.GradleProject;

public final class GradleModelSmoke {
    private static GradleProject find(GradleProject project, String path) {
        if (path.equals(project.getPath())) {
            return project;
        }
        for (GradleProject child : project.getChildren()) {
            GradleProject match = find(child, path);
            if (match != null) {
                return match;
            }
        }
        return null;
    }

    public static void main(String[] args) {
        File root = new File(args[0]);
        ProjectConnection connection = GradleConnector.newConnector()
                .forProjectDirectory(root)
                .connect();
        try {
            GradleProject model = connection.getModel(GradleProject.class);
            if (!"markdown-core".equals(model.getName())) {
                throw new IllegalStateException("Unexpected root model: " + model.getName());
            }

            if (find(model, ":android") != null) {
                throw new IllegalStateException("Gradle model still contains retired :android");
            }
            if (find(model, ":packages:kotlin-markdown-core") == null) {
                throw new IllegalStateException("Gradle model is missing the Kotlin package");
            }
            if (find(model, ":packages:kotlin-markdown-core:android-runtime") == null) {
                throw new IllegalStateException("Gradle model is missing the internal Android runtime");
            }

            System.out.println("Loaded Gradle Tooling API model for " + model.getName());
        } finally {
            connection.close();
        }
    }
}
JAVA

if [ -n "${JAVA_HOME:-}" ]; then
    JAVAC="$JAVA_HOME/bin/javac"
    JAVA="$JAVA_HOME/bin/java"
else
    JAVAC=$(command -v javac)
    JAVA=$(command -v java)
fi

"$JAVAC" -cp "$tooling_jar" -d "$temp_dir" "$temp_dir/GradleModelSmoke.java"
"$JAVA" -cp "$temp_dir:$gradle_home/lib/*:$gradle_home/lib/plugins/*" \
    GradleModelSmoke "$ROOT_DIR"
