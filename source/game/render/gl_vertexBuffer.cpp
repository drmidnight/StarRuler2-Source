#include "vertexBuffer.h"
#include "render/driver.h"
#include "main/references.h"
#include "render/shader_shim.h"
#include "compat/gl.h"
#include <deque>

namespace render {

	extern const RenderMesh* lastRenderedMesh;
	extern float* shaderUniforms;
	extern bool useVertexArrays();
	void glPrepareDraw();

	//Batches stream through a ring of buffers so a new upload never waits on a draw in flight
	static std::deque<GLuint> vertBuffer, indBuffer;
	//Marks the bound buffers as not belonging to any mesh
	static char batchMesh;

	void glDrawBatch(const VertexTCV* vertices, unsigned vertexCount,
		const unsigned short* indices, unsigned indexCount,
		const RenderStep* steps, unsigned stepCount)
	{
		if(vertBuffer.empty()) {
			GLuint buffs[24];
			glGenBuffers(24, buffs);
			for(unsigned i = 0; i < 12; ++i)
				vertBuffer.push_back(buffs[i]);
			for(unsigned i = 12; i < 24; ++i)
				indBuffer.push_back(buffs[i]);
		}

		{
			lastRenderedMesh = reinterpret_cast<const render::RenderMesh*>(&batchMesh);
			if(useVertexArrays())
				glBindVertexArray(0);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indBuffer.front());
			glBindBuffer(GL_ARRAY_BUFFER, vertBuffer.front());

			indBuffer.push_back(indBuffer.front());
			indBuffer.pop_front();
			vertBuffer.push_back(vertBuffer.front());
			vertBuffer.pop_front();

			//Shaders read the generic attributes, and drawing without one reads the
			//fixed-function arrays, so both point at the same data
			glDisableVertexAttribArray(SA_Normal);
			glDisableVertexAttribArray(SA_Tangent);
			glDisableVertexAttribArray(SA_UV2);
			glVertexAttrib4f(SA_Normal, 0.f, 0.f, 1.f, 0.f);

			glEnableVertexAttribArray(SA_Vertex);
			glEnableVertexAttribArray(SA_UV);
			glEnableVertexAttribArray(SA_Color);
			glVertexAttribPointer(SA_Vertex, 3, GL_FLOAT, GL_FALSE, sizeof(VertexTCV), (void*)offsetof(VertexTCV, pos));
			glVertexAttribPointer(SA_UV, 2, GL_FLOAT, GL_FALSE, sizeof(VertexTCV), (void*)offsetof(VertexTCV, uv));
			glVertexAttribPointer(SA_Color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexTCV), (void*)offsetof(VertexTCV, col));

			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_COLOR_ARRAY);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);

			glTexCoordPointer(2, GL_FLOAT, sizeof(VertexTCV), (void*)offsetof(VertexTCV, uv));
			glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(VertexTCV), (void*)offsetof(VertexTCV, col));
			glVertexPointer(3, GL_FLOAT, sizeof(VertexTCV), (void*)offsetof(VertexTCV, pos));
		}

		glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(unsigned short),
					 indices, GL_STREAM_DRAW);

		glBufferData(GL_ARRAY_BUFFER, vertexCount * sizeof(VertexTCV), vertices, GL_STREAM_DRAW);

		for(unsigned i = 0; i < stepCount; ++i) {
			auto& step = steps[i];
			shaderUniforms = step.shaderCache;
			devices.render->switchToRenderState(*step.material);
			glPrepareDraw();
			unsigned count = ((i+1) < stepCount ? steps[i+1].indexOffset : indexCount) - step.indexOffset;

			glDrawRangeElements(step.lines ? GL_LINES : GL_TRIANGLES, step.vertStart, step.vertEnd,
				count, GL_UNSIGNED_SHORT, (void*)(step.indexOffset * sizeof(unsigned short)));
		}

		shaderUniforms = 0;
	}

};
