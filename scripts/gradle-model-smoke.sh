#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

if [ -z "${JAVA_HOME:-}" ] && [ -x "/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/java" ]; then
    JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
    export JAVA_HOME
fi
if [ -z "${ANDROID_HOME:-}" ] && [ -d "$HOME/Library/Android/sdk" ]; then
    ANDROID_HOME="$HOME/Library/Android/sdk"
    export ANDROID_HOME
fi

# Download and validate the pinned distribution before locating its Tooling API.
scripts/gradle.sh --version >/dev/null

GRADLE_USER_HOME=${GRADLE_USER_HOME:-$HOME/.gradle}
tooling_jar=$(find "$GRADLE_USER_HOME/wrapper/dists" \
    -path '*/gradle-9.6.1/lib/gradle-tooling-api-9.6.1.jar' \
    -print | head -n 1)

if [ -z "$tooling_jar" ]; then
    echo "Gradle Tooling API 9.6.1 was not found under $GRADLE_USER_HOME/wrapper/dists" >&2
    exit 1
fi

gradle_home=${tooling_jar%/lib/gradle-tooling-api-9.6.1.jar}
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
